/* Copyright (c) 2017 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

//#include "witnessTracker.h"
#include "server.h"
#include "redisassert.h"
#include "bio.h"
#include "sds.h"
#include "MurmurHash3.h"
#include "rifl.h"
#include "timeTrace.h"
#include "udp.h"
#include "witnesscmd.h"

/* functions from aof.c */
struct client *createFakeClient();
void freeFakeClientArgv(struct client *c);
void freeFakeClient(struct client *c);

/*================================= Globals ================================= */

/* Global vars */
#define WITNESS_BATCH_SIZE 20

struct WitnessGcInfo {
    int hashIndex;
    long long clientId;
    long long requestId;
};

/* 0th index is not used. */
struct WitnessGcInfo unsyncedRpcs[WITNESS_BATCH_SIZE] = {{0,0,0}, };
uint32_t unsyncedRpcsSize = 0;

struct WitnessGcBioContext {
    long long maxOpNum;
    sds gcRequest;
};

/*================================= Functions =============================== */
void scheduleFsyncAndWitnessGc() {
    record("start constructing gc RPC.", 0, 0, 0, 0);
    // Create a GC cmds.
    witnesscmd_t* cmd = (witnesscmd_t *) zmalloc(sizeof(witnesscmd_t));
    uint64_t clientIds[unsyncedRpcsSize];
    uint64_t reqIds[unsyncedRpcsSize];
    uint32_t hashIdxs[unsyncedRpcsSize];
    uint32_t valueSize;
    for (uint32_t i = 0; i < unsyncedRpcsSize; ++i) {
        clientIds[i] = unsyncedRpcs[i].clientId;
        reqIds[i] = unsyncedRpcs[i].requestId;
        hashIdxs[i] = unsyncedRpcs[i].hashIndex;
    }

    // Compute the size of the payload.
    valueSize = (2 * sizeof(uint64_t)) + sizeof(uint32_t);
    valueSize *= unsyncedRpcsSize;

    // Create the packet.
    create_del_wcmd(cmd, clientIds, reqIds, hashIdxs, valueSize);
    unsyncedRpcsSize = 0;
    record("constructed gc RPC.", 0, 0, 0, 0);

    // Submit the GC job to the background thread.
    bioCreateBackgroundJob(BIO_FSYNC_AND_GC_WITNESS,
        cmd,
        (void*)(long)server.aof_fd, server.currentOpNum);
    record("bioBackgroundJob Created.", 0, 0, 0, 0);
}

void trackUnsyncedRpc(client *c) {
    record("tracking UnsyncedRpc", 0, 0, 0, 0);
    unsyncedRpcs[unsyncedRpcsSize].clientId = c->clientId;
    unsyncedRpcs[unsyncedRpcsSize].requestId = c->requestId;
    uint32_t keyHash;
    MurmurHash3_x86_32(c->argv[1]->ptr, sdslen(c->argv[1]->ptr), c->db->id, &keyHash);
//    serverLog(LL_NOTICE, "dictid: %d, key: %s keyLen: %d", c->db->id, (sds)c->argv[1]->ptr, sdslen(c->argv[1]->ptr));
    unsyncedRpcs[unsyncedRpcsSize].hashIndex = keyHash & 1023;
    ++unsyncedRpcsSize;
    record("tracking done", 0, 0, 0, 0);

    if (unsyncedRpcsSize == WITNESS_BATCH_SIZE) {
        scheduleFsyncAndWitnessGc();
    }
}

void witnessListChanged();

bool recoverFromWitness() {
    for (int i = 0; i < server.numWitness; ++i) {
        // Send command.
        char* masterIdxStr = "1";
        sds cmdstr = sdscatprintf(sdsempty(),
                "*2\r\n$16\r\nWGETRECOVERYDATA\r\n$%d\r\n%s\r\n",
                (int)strlen(masterIdxStr), masterIdxStr);
        if (anetWrite(server.fdToWitness[i], cmdstr, sdslen(cmdstr)) == -1) {
            serverLog(LL_WARNING, "Error while sending WGETRECOVERYDATA. %s", strerror(errno));
            continue;
        }

        // Receive response.
        struct client *fakeClient;
        fakeClient = createFakeClient();

        FILE *fp = fdopen(server.fdToWitness[i], "r");
        int count, filteredByRifl = 0;
        char buf[50];
        if (fgets(buf, sizeof(buf), fp) == NULL)
            goto readerr;
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        count = atoi(buf+1);

//        char* data = zmalloc(totalSize + 1);
//        if (fread(data, 1, totalSize, fp) != totalSize)
//            goto readerr;

        for (int reqIdx = 0; reqIdx < count; ++reqIdx) {
            int argc, j;
            unsigned long len;
            robj **argv;
            char buf[128];
            sds argsds;
            struct redisCommand *cmd;

            if (fgets(buf,sizeof(buf),fp) == NULL) {
                if (feof(fp))
                    break;
                else
                    goto readerr;
            }
            if (buf[0] != '*') goto fmterr;
            if (buf[1] == '\0') goto readerr;
            argc = atoi(buf+1);
            if (argc < 1) goto fmterr;

            argv = zmalloc(sizeof(robj*)*argc);
            fakeClient->argc = argc;
            fakeClient->argv = argv;

            for (j = 0; j < argc; j++) {
                if (fgets(buf,sizeof(buf),fp) == NULL) {
                    fakeClient->argc = j; /* Free up to j-1. */
                    freeFakeClientArgv(fakeClient);
                    goto readerr;
                }
                if (buf[0] != '$') goto fmterr;
                len = strtol(buf+1,NULL,10);
                argsds = sdsnewlen(NULL,len);
                if (len && fread(argsds,len,1,fp) == 0) {
                    sdsfree(argsds);
                    fakeClient->argc = j; /* Free up to j-1. */
                    freeFakeClientArgv(fakeClient);
                    goto readerr;
                }
                argv[j] = createObject(OBJ_STRING,argsds);
                if (fread(buf,2,1,fp) == 0) {
                    fakeClient->argc = j+1; /* Free up to j. */
                    freeFakeClientArgv(fakeClient);
                    goto readerr; /* discard CRLF */
                }
            }

            /* Command lookup */
            cmd = lookupCommand(argv[0]->ptr);
            if (!cmd) {
                serverLog(LL_WARNING,"Unknown command '%s' recovery data from witness", (char*)argv[0]->ptr);
                exit(1);
            }

            // RIFL check.
            if (cmd->flags & CMD_AT_MOST_ONCE) {
                getLongLongFromObject(argv[argc-2], &fakeClient->clientId);
                getLongLongFromObject(argv[argc-1], &fakeClient->requestId);
                if (!riflCheckClientIdOk(fakeClient)) {
                    serverLog(LL_WARNING,"Detected RIFL client ID collision.");
                    continue;
                }
                if (riflCheckDuplicate(fakeClient->clientId, fakeClient->requestId)){
                    ++filteredByRifl;
                    goto rifl_duplicate;
                }
            }

            /* Run the command in the context of a fake client */
            cmd->proc(fakeClient);

            /* The fake client should not have a reply */
            serverAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
            /* The fake client should never get blocked */
            serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

    rifl_duplicate:
            /* Clean up. Command code may have changed argv/argc so we use the
             * argv/argc of the client instead of the local variables. */
            freeFakeClientArgv(fakeClient);
        }
        // Recovery completed.
        serverLog(LL_NOTICE, "Recovered state from witness data. (Found: %d, "
                "filtered by RIFL: %d)", count, filteredByRifl);
        return true;

    readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
        if (!feof(fp)) {
            if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
            serverLog(LL_WARNING,"Unrecoverable error reading witness recovery data socket: %s", strerror(errno));
            exit(1);
        }

    fmterr: /* Format error. */
        if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
        serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
        exit(1);
    }
    serverLog(LL_WARNING, "Could not find and recover from any witnesses.");
    return false;
}
