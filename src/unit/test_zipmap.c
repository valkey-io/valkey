#include "../zipmap.c"
#include "test_help.h"

static void zipmapRepr(unsigned char *p) {
    unsigned int l;

    p++;
    while(1) {
        if (p[0] == ZIPMAP_END) {
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                while(e--);
            }
        }
    }
}

int test_zipmapLookUpLargeKey(int argc, char *argv[], int flags) {
    unsigned char *zm;
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    unsigned char buf[512];
    unsigned char *value;
    unsigned int vlen, i;
    for (i = 0; i < 512; i++) buf[i] = 'a';
    zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
    if (zipmapGet(zm,buf,512,&value,&vlen)) {
        TEST_ASSERT(4 == vlen);
        TEST_ASSERT(strncmp("long", (const char*)value, vlen) == 0);
    }
    zfree(zm);
    return 0;
}

int test_zipmapPerformDirectLookup(int argc, char *argv[], int flags) {
    unsigned char *zm;
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);
    unsigned char *value;
    unsigned int vlen;

    if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
        TEST_ASSERT(5 == vlen);
        TEST_ASSERT(!strncmp("12345", (const char*)value,vlen));
    }
    zfree(zm);
    return 0;
}

int test_zipmapIterateThroughElements(int argc, char *argv[], int flags) {
    unsigned char *zm;
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    unsigned char *i = zipmapRewind(zm);
    unsigned char *key, *value;
    unsigned int klen, vlen;
    char* expected_key[] = {"name", "surname", "age", "hello", "foo", "noval"};
    char* expected_value[] = {"foo", "foo", "foo", "world!", "12345", NULL};
    unsigned int expected_klen[] = {4,7,3,5,3,5};
    unsigned int expected_vlen[] = {3,3,3,6,5,0};
    int iter = 0;

    while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
        char *tmp = expected_key[iter];
        TEST_ASSERT(klen == expected_klen[iter]);
        TEST_ASSERT(strncmp((const char*)tmp, (const char*)key, klen) == 0);
        tmp = expected_value[iter];
        TEST_ASSERT(vlen == expected_vlen[iter]);
        TEST_ASSERT(strncmp((const char*)tmp, (const char*)value, vlen) == 0);
        iter++;
    }
    zfree(zm);
    return 0;
}
