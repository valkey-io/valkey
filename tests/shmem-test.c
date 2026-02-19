/*
 * Simple test client for shared memory connection
 * Compile: gcc -o shmem-test shmem-test.c -I../deps/libvalkey/include -L../deps/libvalkey -lvalkey
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "valkey/valkey.h"

int main(int argc, char **argv) {
    valkeyContext *c;
    valkeyReply *reply;
    const char *shm_name = "/valkey_shmem_test";
    
    if (argc > 1) {
        shm_name = argv[1];
    }
    
    printf("Connecting to Valkey via shared memory: %s\n", shm_name);
    
    // For shared memory, we use the shm name as the "hostname"
    // Port is ignored for shmem connections
    c = valkeyConnect(shm_name, 0);
    
    if (c == NULL || c->err) {
        if (c) {
            printf("Connection error: %s\n", c->errstr);
            valkeyFree(c);
        } else {
            printf("Connection error: can't allocate valkey context\n");
        }
        exit(1);
    }
    
    printf("Connected successfully!\n");
    
    // Test PING
    reply = valkeyCommand(c, "PING");
    printf("PING: %s\n", reply->str);
    freeReplyObject(reply);
    
    // Test SET
    reply = valkeyCommand(c, "SET %s %s", "foo", "hello_shmem");
    printf("SET: %s\n", reply->str);
    freeReplyObject(reply);
    
    // Test GET
    reply = valkeyCommand(c, "GET foo");
    printf("GET foo: %s\n", reply->str);
    freeReplyObject(reply);
    
    // Cleanup
    valkeyFree(c);
    
    return 0;
}
