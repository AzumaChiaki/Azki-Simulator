/* SPDX-License-Identifier: GPL-2.0-or-later
 * Actual P65 partition validators/installer; host RAM only, no guest. */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bes-nor-store.h"
#define MiB (1024u * 1024u)
typedef void Object;
typedef const char Error;
static void error_setg(Error **error, const char *message) { if (error) { *error=message; } }
static uint32_t ldl_le_p(const void *v) { const uint8_t *p=v; return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
#include "p65-storage-functions.inc"
static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static void factory_contract(void) {
    BesNorLayout layout; off_t offset=-1;
    CHECK(bes_nor_layout_init(&layout,16*MiB,0xc0000,0xcc0000,0xcc0000,0,0));
    CHECK(bes_nor_layout_bytes(&layout)==0x340000);
    CHECK(bes_nor_layout_translate(&layout,0xcc0000,256*1024,&offset) && offset==0);
    CHECK(bes_nor_layout_translate(&layout,0xd00000,3*MiB,&offset) && offset==256*1024);
    CHECK(!bes_nor_layout_translate(&layout,0xcbffff,1,&offset));
    CHECK(!bes_nor_layout_translate(&layout,0xffffff,2,&offset));
    CHECK(!bes_nor_layout_init(&layout,16*MiB,0xc0000,0xcc0000,0xfe0000,0,0));
    CHECK(bes_nor_layout_init(&layout,16*MiB,0xb0000,0xfc0000,0xfe0000,0,0)); /* Q65 unchanged. */
    uint8_t sentinel=0x65; Error *error=NULL;
    best1503_set_factory_image((Object *)&sentinel,"unopened-factory.bin",&error);
    CHECK(error && strstr(error,"unverified"));
    CHECK(sentinel==0x65);
}
static void factory_writeback(const char *path) {
    BesNorLayout layout; BesNorStore store;
    uint8_t *flash=g_malloc(16*MiB); memset(flash,0x65,16*MiB);
    CHECK(bes_nor_layout_init(&layout,16*MiB,0xc0000,0xcc0000,0xcc0000,0,0));
    CHECK(bes_nor_store_open(&store,path,&layout,flash));
    CHECK(flash[0xcbffff]==0x65 && flash[0xcc0000]==0xff && flash[0xffffff]==0xff);
    const uint8_t start[]={0x0f,0xa5}, end[]={0x45,0x67}, audio[]={0x1c,0xec,0x57,0xbe};
    CHECK(bes_nor_store_flush(&store,0xcc0000,sizeof(start),start));
    CHECK(bes_nor_store_flush(&store,0xcffffe,sizeof(end),end));
    CHECK(bes_nor_store_flush(&store,0xd00000,sizeof(audio),audio));
    bes_nor_store_close(&store);
    CHECK(bes_nor_store_open(&store,path,&layout,flash));
    CHECK(!memcmp(flash+0xcc0000,start,sizeof(start)) && !memcmp(flash+0xcffffe,end,sizeof(end)));
    CHECK(flash[0xcbffff]==0x65 && flash[0xcc0002]==0xff && flash[0xcffffd]==0xff);
    CHECK(!memcmp(flash+0xd00000,audio,sizeof(audio)) && flash[0xd00004]==0xff);
    CHECK(!bes_nor_store_flush(&store,0xcbffff,1,start));
    CHECK(store.failed);
    bes_nor_store_close(&store); g_free(flash);
}
static void install_vectors(void) {
    const size_t flash_bytes=16*MiB, offset=0xd00000, capacity=3*MiB;
    uint8_t *flash=g_malloc(flash_bytes), *image=g_malloc(capacity);
    memset(flash,0xff,flash_bytes); memset(image,0x7b,capacity);
    memcpy(image,"\x1c\xec\x57\xbe",4);
    flash[offset-1]=0x35; const char *error=NULL;
    CHECK(p65_install_audio(flash,flash_bytes,image,0x128,&error));
    CHECK(!memcmp(flash+offset,image,0x128));
    CHECK(flash[offset-1]==0x35 && flash[offset+0x128]==0xff && flash[flash_bytes-1]==0xff);
    CHECK(p65_install_audio(flash,flash_bytes,image,0x128,&error)); /* Same restored content is safe. */
    image[100]^=1;
    CHECK(!p65_install_audio(flash,flash_bytes,image,0x128,&error));
    CHECK(strstr(error,"conflicts") && flash[offset+100]!=(image[100]) && flash[offset-1]==0x35);
    image[100]^=1; flash[flash_bytes-1]=0;
    CHECK(!p65_install_audio(flash,flash_bytes,image,0x128,&error)); /* Different trailing contents. */
    CHECK(flash[flash_bytes-1]==0);
    memset(flash+offset,0xff,capacity);
    CHECK(p65_install_audio(flash,flash_bytes,image,capacity,&error));
    CHECK(!memcmp(flash+offset,image,capacity) && flash[offset-1]==0x35);
    CHECK(!p65_install_audio(flash,flash_bytes,image,capacity+1,&error));
    CHECK(!p65_install_audio(flash,flash_bytes,image,0x127,&error));
    CHECK(!p65_install_audio(flash,flash_bytes-1,image,0x128,&error));
    CHECK(!p65_install_audio(NULL,flash_bytes,image,0x128,&error));
    CHECK(!p65_install_audio(flash,flash_bytes,NULL,0x128,&error));
    image[0]=0;
    CHECK(!p65_install_audio(flash,flash_bytes,image,0x128,&error));
    CHECK(ldl_le_p(flash+offset)==0xbe57ec1c); /* Rejected inputs leave prior image intact. */
    g_free(image); g_free(flash);
}
static void resource_boundaries(void) {
    uint8_t header[16]="-rom1fs-";
    CHECK(p65_resource_image_valid(header,16));
    CHECK(p65_resource_image_valid(header,220*MiB));
    CHECK(!p65_resource_image_valid(header,220*MiB+1));
    CHECK(!p65_resource_image_valid(header,235*MiB));
    CHECK(!p65_resource_image_valid(header,15));
    CHECK(!p65_resource_image_valid(NULL,16));
    header[0]=0;
    CHECK(!p65_resource_image_valid(header,16));
}
static void original_audio(const char *path) {
    char *audio=NULL; gsize bytes=0;
    CHECK(g_file_get_contents(path,&audio,&bytes,NULL));
    uint8_t *flash=g_malloc(16*MiB); memset(flash,0xff,16*MiB);
    memset(flash,0x69,0xd00000); /* Factory/AP neighborhood sentinel. */
    const char *error=NULL;
    CHECK(p65_install_audio(flash,16*MiB,(uint8_t *)audio,bytes,&error));
    CHECK(!memcmp(flash+0xd00000,audio,bytes));
    bool prefix=true, padding=true;
    for (size_t i=0;i<0xd00000;i++) { prefix &= flash[i]==0x69; }
    for (size_t i=0xd00000+bytes;i<16*MiB;i++) { padding &= flash[i]==0xff; }
    CHECK(prefix && padding);
    printf("Original P65 audio: %zu bytes preserved at NOR offset 0xd00000\n",bytes);
    g_free(flash); g_free(audio);
}
int main(int argc,char **argv) {
    factory_contract(); install_vectors(); resource_boundaries();
    const char *nor_path=getenv("AZKI_P65_TEST_NOR");
    if (nor_path) { factory_writeback(nor_path); }
    if (argc==2) { original_audio(argv[1]); }
    printf("P65 storage production contract: %u checks passed; no CP execution\n",checks);
    return 0;
}
