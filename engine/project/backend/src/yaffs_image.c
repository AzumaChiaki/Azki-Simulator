/* Build an in-band-tag YAFFS2 image through the upstream filesystem API.
 * GPL-2.0-only, linked against Aleph One YAFFS2 (see references/yaffs2/LICENSE).
 * Also remount and compare every file before publishing the image.
 */
#include <assert.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "windows_helper_io.h"
#endif
#include "yaffsfs.h"
#include "yaffs_guts.h"
#include "yaffs_trace.h"

unsigned yaffs_trace_mask;
static int last_error;
static unsigned char *storage;
static size_t storage_size;
void yaffsfs_Lock(void) {}
void yaffsfs_Unlock(void) {}
void yaffsfs_OSInitialisation(void) {}
void yaffsfs_SetError(int err) { last_error=err; }
int yaffsfs_GetLastError(void) { return last_error; }
u32 yaffsfs_CurrentTime(void) { return 1700000000; }
void *yaffsfs_malloc(size_t size) { return malloc(size); }
void yaffsfs_free(void *p) { free(p); }
void yaffsfs_get_malloc_values(unsigned *a,unsigned *b) { if(a)*a=0;if(b)*b=0; }
int yaffsfs_CheckMemRegion(const void *p,size_t n,int wr) { return p?0:-1; }
void yaffs_bug_fn(const char *file,int line) { fprintf(stderr,"YAFFS BUG %s:%d\n",file,line);abort(); }
static int init(struct yaffs_dev *d) { return YAFFS_OK; }
static int bad(struct yaffs_dev *d,int block) { return block>=0 && (size_t)block*131072<storage_size ? YAFFS_OK:YAFFS_FAIL; }
static int mark_bad(struct yaffs_dev *d,int block) { return YAFFS_FAIL; }
static int erase(struct yaffs_dev *d,int block) {
    if(bad(d,block)!=YAFFS_OK)return YAFFS_FAIL;
    memset(storage+(size_t)block*131072,255,131072);return YAFFS_OK;
}
static int read_chunk(struct yaffs_dev *d,int chunk,u8 *data,int len,u8 *oob,int olen,enum yaffs_ecc_result *ecc) {
    if(chunk<0 || len<0 || len>2048 || olen || (size_t)chunk*2048+len>storage_size)return YAFFS_FAIL;
    if(data)memcpy(data,storage+(size_t)chunk*2048,len);
    if(ecc)*ecc=YAFFS_ECC_RESULT_NO_ERROR;return YAFFS_OK;
}
static int write_chunk(struct yaffs_dev *d,int chunk,const u8 *data,int len,const u8 *oob,int olen) {
    if(chunk<0 || len<0 || len>2048 || olen || !data || (size_t)chunk*2048+len>storage_size)return YAFFS_FAIL;
    unsigned char *dest=storage+(size_t)chunk*2048;
    for(int i=0;i<len;i++) { if((dest[i]&data[i])!=data[i])return YAFFS_FAIL; dest[i]&=data[i]; }
    return YAFFS_OK;
}
static void check(int ok,const char *what) { if(!ok) {fprintf(stderr,"%s failed (YAFFS error %d)\n",what,last_error);exit(1);} }
struct source_item { char *host; char *dest; int directory; };
static struct source_item *items;
static size_t item_count, file_count;
static void add_item(const char *host, const char *dest, int directory) {
    check(strlen(dest)<1024,"destination length");
    for(size_t i=0;i<item_count;i++) check(strcmp(items[i].dest,dest),"duplicate destination");
    struct source_item *next=realloc(items,(item_count+1)*sizeof(*items));check(next!=NULL,"item allocation");
    items=next;items[item_count++]=(struct source_item){strdup(host),strdup(dest),directory};
    check(items[item_count-1].host && items[item_count-1].dest,"path allocation");
    if(!directory)file_count++;
}
#ifdef _WIN32
static int name_compare(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void collect_tree(const char *host, const char *dest, unsigned depth) {
    check(depth<32,"tree depth");
    HANDLE directory=azki_helper_directory(host);check(directory!=INVALID_HANDLE_VALUE,"private source directory without reparse");
    AzkiWinPath checked;check(azki_win_path_open(&checked,host),"source tree path");
    size_t length=wcslen(checked.name);wchar_t *pattern=malloc((length+3)*sizeof(wchar_t));check(pattern!=NULL,"tree pattern allocation");
    memcpy(pattern,checked.name,length*sizeof(wchar_t));wcscpy(pattern+length,L"\\*");
    WIN32_FIND_DATAW data;HANDLE search=FindFirstFileW(pattern,&data);DWORD error=GetLastError();
    free(pattern);azki_win_path_close(&checked);
    char **names=NULL;size_t count=0;
    if(search==INVALID_HANDLE_VALUE)check(error==ERROR_FILE_NOT_FOUND,"enumerate source directory");
    else {
        do {
            if(!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L".."))continue;
            check(!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT),"source reparse point rejected");
            char *name=azki_helper_utf8(data.cFileName);check(name!=NULL,"source filename UTF-8");
            check(*name && strlen(name)<=255 && !strchr(name,'/'),"tree filename");
            char **next=realloc(names,(count+1)*sizeof(*names));check(next!=NULL,"directory allocation");names=next;names[count++]=name;
        } while(FindNextFileW(search,&data));
        error=GetLastError();FindClose(search);check(error==ERROR_NO_MORE_FILES,"complete directory enumeration");
    }
    if(count>1)qsort(names,count,sizeof(*names),name_compare);
    for(size_t i=0;i<count;i++) {
        char child[4096],target[1024];
        check(snprintf(child,sizeof(child),"%s/%s",host,names[i])<(int)sizeof(child),"source path length");
        check(snprintf(target,sizeof(target),"%s/%s",dest,names[i])<(int)sizeof(target),"target path length");
        AzkiWinPath childpath;check(azki_win_path_open(&childpath,child),"source child path");
        DWORD attributes=GetFileAttributesW(childpath.name);azki_win_path_close(&childpath);
        check(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"real source child");
        int is_directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        if(is_directory) {HANDLE h=azki_helper_directory(child);check(h!=INVALID_HANDLE_VALUE,"private child directory");CloseHandle(h);}
        else {int fd=azki_helper_open(child,false,false,false);check(fd>=0,"private regular single-link source");_close(fd);}
        add_item(child,target,is_directory);if(is_directory)collect_tree(child,target,depth+1);free(names[i]);
    }
    free(names);CloseHandle(directory);
}
#else
static void collect_tree(const char *host, const char *dest, unsigned depth) {
    check(depth<32,"tree depth");
    struct dirent **entries;
    int n=scandir(host,&entries,NULL,alphasort);check(n>=0,"source directory");
    for(int i=0;i<n;i++) {
        const char *name=entries[i]->d_name;
        if(!strcmp(name,".") || !strcmp(name,"..")) { free(entries[i]);continue; }
        check(*name && strlen(name)<=255 && !strchr(name,'/'),"tree filename");
        char child[4096], target[1024];
        check(snprintf(child,sizeof(child),"%s/%s",host,name)<(int)sizeof(child),"source path length");
        check(snprintf(target,sizeof(target),"%s/%s",dest,name)<(int)sizeof(target),"target path length");
        struct stat st;check(lstat(child,&st)==0,"source stat");
        check(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode),"only regular files and directories are accepted");
        add_item(child,target,S_ISDIR(st.st_mode));
        if(S_ISDIR(st.st_mode))collect_tree(child,target,depth+1);
        free(entries[i]);
    }
    free(entries);
}
#endif
#ifdef _WIN32
static FILE *open_source(const char *path) {FILE *stream=azki_helper_stream(path,false);check(stream!=NULL,"private source stream");return stream;}
#else
static FILE *open_source(const char *path) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW);check(fd>=0,"open source without symlink");
    struct stat st;check(fstat(fd,&st)==0 && S_ISREG(st.st_mode),"regular source");
    FILE *stream=fdopen(fd,"rb");check(stream!=NULL,"source stream");return stream;
}
#endif
int main(int argc,char **argv) {
    if(argc<4) {fprintf(stderr,"Usage: yaffs-image output megabytes flat-file...\n       yaffs-image output --bytes byte-count --tree directory\n");return 2;}
    char *end;int first=3;
    if(!strcmp(argv[2],"--bytes")) {
        check(argc>=5,"byte size argument");
        storage_size=strtoull(argv[3],&end,10);first=4;
    } else {
        unsigned long mb=strtoul(argv[2],&end,10);
        check(mb<=512,"image size");storage_size=mb*1024*1024;
    }
    check(!*end && storage_size>=8*1024*1024 && storage_size<=512*1024*1024 &&
          storage_size%131072==0,"erase-aligned image size");
    check(first<argc,"source arguments");
    if(!strcmp(argv[first],"--tree")) {
        check(argc==first+2,"one source tree required");
#ifdef _WIN32
        HANDLE root=azki_helper_directory(argv[first+1]);check(root!=INVALID_HANDLE_VALUE,"tree root must be private real directory");CloseHandle(root);
#else
        struct stat st;check(lstat(argv[first+1],&st)==0 && S_ISDIR(st.st_mode),"tree root must be a real directory");
#endif
        collect_tree(argv[first+1],"/nand",0);
    } else {
        for(int i=first;i<argc;i++) {
            const char *name=strrchr(argv[i],'/');
#ifdef _WIN32
            const char *backslash=strrchr(argv[i],'\\');if(backslash && (!name || backslash>name))name=backslash;
#endif
            name=name?name+1:argv[i];
            check(strlen(name)<=255 && *name && strcmp(name,".") && strcmp(name,".."),"filename");
            char dest[300];snprintf(dest,sizeof(dest),"/nand/%s",name);add_item(argv[i],dest,0);
        }
    }
    check(file_count>0,"at least one file required");
    storage=malloc(storage_size);check(storage!=NULL,"allocation");
    memset(storage,255,storage_size);
    struct yaffs_dev dev={0};
    dev.param.name="/nand";dev.param.inband_tags=1;dev.param.total_bytes_per_chunk=2048;
    dev.param.chunks_per_block=64;dev.param.start_block=0;dev.param.end_block=storage_size/131072-1;
    dev.param.n_reserved_blocks=5;dev.param.n_caches=10;dev.param.is_yaffs2=1;dev.param.use_nand_ecc=1;
    dev.param.skip_checkpt_rd=1;dev.param.skip_checkpt_wr=1;dev.param.stored_endian=1;
    dev.drv.drv_initialise_fn=init;dev.drv.drv_deinitialise_fn=init;dev.drv.drv_read_chunk_fn=read_chunk;
    dev.drv.drv_write_chunk_fn=write_chunk;dev.drv.drv_erase_fn=erase;
    dev.drv.drv_check_bad_fn=bad;dev.drv.drv_mark_bad_fn=mark_bad;
    yaffs_add_device(&dev);check(yaffs_mount("/nand")==0,"mount");
    unsigned char buf[65536],actual[65536];
    for(int pass=0;pass<2;pass++) {
        for(size_t i=0;i<item_count;i++) {
            const char *dest=items[i].dest;
            if(items[i].directory) {
                if(!pass)check(yaffs_mkdir(dest,0755)==0,"create directory");
                else {struct yaffs_stat st;check(yaffs_stat(dest,&st)==0 && S_ISDIR(st.st_mode),"directory readback");}
                continue;
            }
            FILE *src=open_source(items[i].host);
            /* The other YAFFS translation units use yportenv's Linux flags;
             * the host open_source() above uses macOS fcntl flags instead. */
            int fd=yaffs_open(dest,pass?0:(01|0100|0200),0644);check(fd>=0,dest);
            size_t n;
            while((n=fread(buf,1,sizeof(buf),src))) {
                if(!pass)check(yaffs_write(fd,buf,n)==(int)n,"write");
                else { check(yaffs_read(fd,actual,n)==(int)n,"readback");check(!memcmp(buf,actual,n),"content compare"); }
            }
            check(!ferror(src),"source read");fclose(src);
            if(pass)check(yaffs_read(fd,actual,1)==0,"EOF");
            check(yaffs_close(fd)==0,"close");
        }
        check(yaffs_unmount("/nand")==0,"unmount");
        if(!pass)check(yaffs_mount("/nand")==0,"remount");
    }
#ifdef _WIN32
    FILE *out=azki_helper_stream(argv[1],true);check(out!=NULL,"private CREATE_NEW output");
#else
    FILE *out=fopen(argv[1],"wb");check(out!=NULL,"output");
#endif
    check(fwrite(storage,1,storage_size,out)==storage_size,"image write");
#ifdef _WIN32
    check(fflush(out)==0 && azki_win_fsync(_fileno(out))==0,"output flush");
#endif
    check(fclose(out)==0,"output close");
    fprintf(stdout,"Verified %zu files and %zu directories after cold remount, %zu bytes YAFFS2 in-band tags\n",file_count,item_count-file_count,storage_size);
    return 0;
}
