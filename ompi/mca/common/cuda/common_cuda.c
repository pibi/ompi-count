/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * This file contains various support functions for doing CUDA
 * operations.  Some of the features are only available in CUDA 4.1
 * and later, so some code is conditionalized around the
 * OMPI_CUDA_SUPPORT_41 macro.
 */
#include "ompi_config.h"

#include <errno.h>
#include <unistd.h>
#include <cuda.h>

#include "opal/align.h"
#include "opal/datatype/opal_convertor.h"
#include "opal/datatype/opal_datatype_cuda.h"
#include "opal/util/output.h"
#include "opal/util/lt_interface.h"
#include "opal/util/show_help.h"
#include "ompi/mca/mpool/base/base.h"
#include "ompi/mca/rte/rte.h"
#include "ompi/runtime/params.h"
#include "common_cuda.h"

/**
 * Since function names can get redefined in cuda.h file, we need to do this
 * stringifying to get the latest function name from the header file.  For
 * example, cuda.h may have something like this:
 * #define cuMemFree cuMemFree_v2
 * We want to make sure we find cuMemFree_v2, not cuMemFree.
 */
#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)

#define OMPI_CUDA_DLSYM(libhandle, funcName)                                         \
do {                                                                                 \
    *(void **)(&cuFunc.funcName) = opal_lt_dlsym(libhandle, STRINGIFY(funcName));    \
    if (NULL == cuFunc.funcName) {                                                   \
        opal_show_help("help-mpi-common-cuda.txt", "dlsym failed", true,             \
                       STRINGIFY(funcName), opal_lt_dlerror());                      \
        return 1;                                                                    \
    } else {                                                                         \
        opal_output_verbose(15, mca_common_cuda_output,                              \
                            "CUDA: successful dlsym of %s",                          \
                            STRINGIFY(funcName));                                    \
    }                                                                                \
} while (0)

/* Structure to hold CUDA function pointers that get dynamically loaded. */
struct cudaFunctionTable {
    int (*cuPointerGetAttribute)(void *, CUpointer_attribute, CUdeviceptr);
    int (*cuMemcpyAsync)(CUdeviceptr, CUdeviceptr, size_t, CUstream);
    int (*cuMemcpy)(CUdeviceptr, CUdeviceptr, size_t);
    int (*cuMemAlloc)(CUdeviceptr *, unsigned int);
    int (*cuMemFree)(CUdeviceptr buf);
    int (*cuCtxGetCurrent)(void *cuContext);
    int (*cuStreamCreate)(CUstream *, int);
    int (*cuEventCreate)(CUevent *, int);
    int (*cuEventRecord)(CUevent, CUstream);
    int (*cuMemHostRegister)(void *, size_t, unsigned int);
    int (*cuMemHostUnregister)(void *);
    int (*cuEventQuery)(CUevent);
    int (*cuEventDestroy)(CUevent);
    int (*cuStreamWaitEvent)(CUstream, CUevent, unsigned int);
    int (*cuMemGetAddressRange)(CUdeviceptr*, size_t*, CUdeviceptr);
#if OMPI_CUDA_SUPPORT_41
    int (*cuIpcGetEventHandle)(CUipcEventHandle*, CUevent);
    int (*cuIpcOpenEventHandle)(CUevent*, CUipcEventHandle);
    int (*cuIpcOpenMemHandle)(CUdeviceptr*, CUipcMemHandle, unsigned int);
    int (*cuIpcCloseMemHandle)(CUdeviceptr);
    int (*cuIpcGetMemHandle)(CUipcMemHandle*, CUdeviceptr);
#endif /* OMPI_CUDA_SUPPORT_41 */
} cudaFunctionTable;
typedef struct cudaFunctionTable cudaFunctionTable_t;
cudaFunctionTable_t cuFunc;

static bool common_cuda_initialized = false;
static bool common_cuda_init_function_added = false;
static int mca_common_cuda_verbose;
static int mca_common_cuda_output = 0;
static bool mca_common_cuda_enabled = false;
static bool mca_common_cuda_register_memory = true;
static bool mca_common_cuda_warning = false;
static opal_list_t common_cuda_memory_registrations;
static CUstream ipcStream;
static CUstream dtohStream;
static CUstream htodStream;

/* Functions called by opal layer - plugged into opal function table */
static int mca_common_cuda_is_gpu_buffer(const void*);
static int mca_common_cuda_memmove(void*, void*, size_t);
static int mca_common_cuda_cu_memcpy_async(void*, const void*, size_t, opal_convertor_t*);
static int mca_common_cuda_cu_memcpy(void*, const void*, size_t);

/* Structure to hold memory registrations that are delayed until first
 * call to send or receive a GPU pointer */
struct common_cuda_mem_regs_t {
    opal_list_item_t super;
    void *ptr;
    size_t amount;
    char *msg;
};
typedef struct common_cuda_mem_regs_t common_cuda_mem_regs_t;
OBJ_CLASS_DECLARATION(common_cuda_mem_regs_t);
OBJ_CLASS_INSTANCE( common_cuda_mem_regs_t,
                    opal_list_item_t,
                    NULL,
                    NULL );

#if OMPI_CUDA_SUPPORT_41
static int mca_common_cuda_async = 1;

/* Array of CUDA events to be queried for IPC stream, sending side and
 * receiving side. */
CUevent *cuda_event_ipc_array;
CUevent *cuda_event_dtoh_array;
CUevent *cuda_event_htod_array;

/* Array of fragments currently being moved by cuda async non-blocking
 * operations */
struct mca_btl_base_descriptor_t **cuda_event_ipc_frag_array;
struct mca_btl_base_descriptor_t **cuda_event_dtoh_frag_array;
struct mca_btl_base_descriptor_t **cuda_event_htod_frag_array;

/* First free/available location in cuda_event_status_array */
int cuda_event_ipc_first_avail, cuda_event_dtoh_first_avail, cuda_event_htod_first_avail;

/* First currently-being used location in the cuda_event_status_array */
int cuda_event_ipc_first_used, cuda_event_dtoh_first_used, cuda_event_htod_first_used;

/* Number of status items currently in use */
int cuda_event_ipc_num_used, cuda_event_dtoh_num_used, cuda_event_htod_num_used;

/* Size of array holding events */
int cuda_event_max = 200;

/* Handle to libcuda.so */
opal_lt_dlhandle libcuda_handle;

#define CUDA_COMMON_TIMING 0
#if CUDA_COMMON_TIMING
/* Some timing support structures.  Enable this to help analyze
 * internal performance issues. */
static struct timespec ts_start;
static struct timespec ts_end;
static double accum;
#define THOUSAND  1000L
#define MILLION   1000000L
static float mydifftime(struct timespec ts_start, struct timespec ts_end);
#endif /* CUDA_COMMON_TIMING */

static int mca_common_cuda_load_libcuda(void);
/* These functions are typically unused in the optimized builds. */
static void cuda_dump_evthandle(int, void *, char *) __opal_attribute_unused__ ;
static void cuda_dump_memhandle(int, void *, char *) __opal_attribute_unused__ ;
#if OPAL_ENABLE_DEBUG
#define CUDA_DUMP_MEMHANDLE(a) cuda_dump_memhandle a
#define CUDA_DUMP_EVTHANDLE(a) cuda_dump_evthandle a
#else
#define CUDA_DUMP_MEMHANDLE(a)
#define CUDA_DUMP_EVTHANDLE(a)
#endif /* OPAL_ENABLE_DEBUG */

#endif /* OMPI_CUDA_SUPPORT_41 */

int mca_common_cuda_register_mca_variables(void)
{
    static bool registered = false;

    if (registered) {
        return OMPI_SUCCESS;
    }

    registered = true;

    /* Set different levels of verbosity in the cuda related code. */
    mca_common_cuda_verbose = 0;
    (void) mca_base_var_register("ompi", "mpi", "common_cuda", "verbose",
                                 "Set level of common cuda verbosity",
                                 MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                 OPAL_INFO_LVL_9,
                                 MCA_BASE_VAR_SCOPE_READONLY,
                                 &mca_common_cuda_verbose);

    /* Control whether system buffers get CUDA pinned or not.  Allows for 
     * performance analysis. */
    mca_common_cuda_register_memory = true;
    (void) mca_base_var_register("ompi", "mpi", "common_cuda", "register_memory",
                                 "Whether to cuMemHostRegister preallocated BTL buffers",
                                 MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                 OPAL_INFO_LVL_9,
                                 MCA_BASE_VAR_SCOPE_READONLY,
                                 &mca_common_cuda_register_memory);

    /* Control whether we see warnings when CUDA memory registration fails.  This is
     * useful when CUDA support is configured in, but we are running a regular MPI
     * application without CUDA. */
    mca_common_cuda_warning = true;
    (void) mca_base_var_register("ompi", "mpi", "common_cuda", "warning",
                                 "Whether to print warnings when CUDA registration fails",
                                 MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                 OPAL_INFO_LVL_9,
                                 MCA_BASE_VAR_SCOPE_READONLY,
                                 &mca_common_cuda_warning);

#if OMPI_CUDA_SUPPORT_41
    /* Use this flag to test async vs sync copies */
    mca_common_cuda_async = 1;
    (void) mca_base_var_register("ompi", "mpi", "common_cuda", "memcpy_async",
                                 "Set to 0 to force CUDA sync copy instead of async",
                                 MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                 OPAL_INFO_LVL_9,
                                 MCA_BASE_VAR_SCOPE_READONLY,
                                 &mca_common_cuda_async);

    /* Use this parameter to increase the number of outstanding events allows */
    cuda_event_max = 200;
    (void) mca_base_var_register("ompi", "mpi", "common_cuda", "event_max",
                                 "Set number of oustanding CUDA events",
                                 MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                 OPAL_INFO_LVL_9,
                                 MCA_BASE_VAR_SCOPE_READONLY,
                                 &cuda_event_max);
#endif /* OMPI_CUDA_SUPPORT_41 */

    return OMPI_SUCCESS;
}

/**
 * This function is registered with the OPAL CUDA support.  In that way,
 * we will complete initialization when OPAL detects the first GPU memory
 * access.  In the case that no GPU memory access happens, then this function
 * never gets called.
 */
static int mca_common_cuda_init(opal_common_cuda_function_table_t *ftable)
{
    int i, s;
    CUresult res;
    CUcontext cuContext;
    common_cuda_mem_regs_t *mem_reg;

    if (!ompi_mpi_cuda_support) {
        return OMPI_ERROR;
    }

    if (common_cuda_initialized) {
        return OMPI_SUCCESS;
    }

    /* Make sure this component's variables are registered */
    mca_common_cuda_register_mca_variables();

    ftable->gpu_is_gpu_buffer = &mca_common_cuda_is_gpu_buffer;
    ftable->gpu_cu_memcpy_async = &mca_common_cuda_cu_memcpy_async;
    ftable->gpu_cu_memcpy = &mca_common_cuda_cu_memcpy;
    ftable->gpu_memmove = &mca_common_cuda_memmove;

    mca_common_cuda_output = opal_output_open(NULL);
    opal_output_set_verbosity(mca_common_cuda_output, mca_common_cuda_verbose);

    /* If we cannot load the libary, then disable support */
    if (0 != mca_common_cuda_load_libcuda()) {
        common_cuda_initialized = true;
        ompi_mpi_cuda_support = 0;
        return OMPI_ERROR;
    }

    /* Check to see if this process is running in a CUDA context.  If
     * so, all is good.  If not, then disable registration of memory. */
    res = cuFunc.cuCtxGetCurrent(&cuContext);
    if (CUDA_SUCCESS != res) {
        if (mca_common_cuda_warning) {
            /* Check for the not initialized error since we can make suggestions to
             * user for this error. */
            if (CUDA_ERROR_NOT_INITIALIZED == res) {
                opal_show_help("help-mpi-common-cuda.txt", "cuCtxGetCurrent failed not initialized",
                               true);
            } else {
                opal_show_help("help-mpi-common-cuda.txt", "cuCtxGetCurrent failed",
                               true, res);
            }
        }
        mca_common_cuda_enabled = false;
        mca_common_cuda_register_memory = false;
    } else if ((CUDA_SUCCESS == res) && (NULL == cuContext)) {
        if (mca_common_cuda_warning) {
            opal_show_help("help-mpi-common-cuda.txt", "cuCtxGetCurrent returned NULL",
                           true);
        }
        mca_common_cuda_enabled = false;
        mca_common_cuda_register_memory = false;
    } else {
        /* All is good.  mca_common_cuda_register_memory will retain its original
         * value.  Normally, that is 1, but the user can override it to disable
         * registration of the internal buffers. */
        mca_common_cuda_enabled = true;
        opal_output_verbose(20, mca_common_cuda_output,
                            "CUDA: cuCtxGetCurrent succeeded");
    }

    /* No need to go on at this point.  If we cannot create a context and we are at
     * the point where we are making MPI calls, it is time to fully disable
     * CUDA support.
     */
    if (false == mca_common_cuda_enabled) {
        return OMPI_ERROR;
    }

#if OMPI_CUDA_SUPPORT_41
    if (true == mca_common_cuda_enabled) {
        /* Set up an array to store outstanding IPC async copy events */
        cuda_event_ipc_array = NULL;
        cuda_event_ipc_frag_array = NULL;
        cuda_event_ipc_num_used = 0;
        cuda_event_ipc_first_avail = 0;
        cuda_event_ipc_first_used = 0;

        cuda_event_ipc_array = (CUevent *) malloc(sizeof(CUevent) * cuda_event_max);
        if (NULL == cuda_event_ipc_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }

        /* Create the events since they can be reused. */
        for (i = 0; i < cuda_event_max; i++) {
            res = cuFunc.cuEventCreate(&cuda_event_ipc_array[i], CU_EVENT_DISABLE_TIMING);
            if (CUDA_SUCCESS != res) {
                opal_show_help("help-mpi-common-cuda.txt", "cuEventCreate failed",
                               true, res);
                return OMPI_ERROR;
            }
        }

        /* The first available status index is 0.  Make an empty frag
           array. */
        cuda_event_ipc_frag_array = (struct mca_btl_base_descriptor_t **)
            malloc(sizeof(struct mca_btl_base_descriptor_t *) * cuda_event_max);
        if (NULL == cuda_event_ipc_frag_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }
    }

#endif /* OMPI_CUDA_SUPPORT_41 */
    if (true == mca_common_cuda_enabled) {
        /* Set up an array to store outstanding async dtoh events.  Used on the
         * sending side for asynchronous copies. */
        cuda_event_dtoh_array = NULL;
        cuda_event_dtoh_frag_array = NULL;
        cuda_event_dtoh_num_used = 0;
        cuda_event_dtoh_first_avail = 0;
        cuda_event_dtoh_first_used = 0;

        cuda_event_dtoh_array = (CUevent *) malloc(sizeof(CUevent) * cuda_event_max);
        if (NULL == cuda_event_dtoh_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }

        /* Create the events since they can be reused. */
        for (i = 0; i < cuda_event_max; i++) {
            res = cuFunc.cuEventCreate(&cuda_event_dtoh_array[i], CU_EVENT_DISABLE_TIMING);
            if (CUDA_SUCCESS != res) {
                opal_show_help("help-mpi-common-cuda.txt", "cuEventCreate failed",
                               true, res);
                return OMPI_ERROR;
            }
        }

        /* The first available status index is 0.  Make an empty frag
           array. */
        cuda_event_dtoh_frag_array = (struct mca_btl_base_descriptor_t **)
            malloc(sizeof(struct mca_btl_base_descriptor_t *) * cuda_event_max);
        if (NULL == cuda_event_dtoh_frag_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }

        /* Set up an array to store outstanding async htod events.  Used on the
         * receiving side for asynchronous copies. */
        cuda_event_htod_array = NULL;
        cuda_event_htod_frag_array = NULL;
        cuda_event_htod_num_used = 0;
        cuda_event_htod_first_avail = 0;
        cuda_event_htod_first_used = 0;

        cuda_event_htod_array = (CUevent *) malloc(sizeof(CUevent) * cuda_event_max);
        if (NULL == cuda_event_htod_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }

        /* Create the events since they can be reused. */
        for (i = 0; i < cuda_event_max; i++) {
            res = cuFunc.cuEventCreate(&cuda_event_htod_array[i], CU_EVENT_DISABLE_TIMING);
            if (CUDA_SUCCESS != res) {
                opal_show_help("help-mpi-common-cuda.txt", "cuEventCreate failed",
                               true, res);
                return OMPI_ERROR;
            }
        }

        /* The first available status index is 0.  Make an empty frag
           array. */
        cuda_event_htod_frag_array = (struct mca_btl_base_descriptor_t **)
            malloc(sizeof(struct mca_btl_base_descriptor_t *) * cuda_event_max);
        if (NULL == cuda_event_htod_frag_array) {
            opal_show_help("help-mpi-common-cuda.txt", "No memory",
                           true, errno, strerror(errno));
            return OMPI_ERROR;
        }
    }

    s = opal_list_get_size(&common_cuda_memory_registrations);
    for(i = 0; i < s; i++) {
        mem_reg = (common_cuda_mem_regs_t *)
            opal_list_remove_first(&common_cuda_memory_registrations);
        if (mca_common_cuda_enabled && mca_common_cuda_register_memory) {
            res = cuFunc.cuMemHostRegister(mem_reg->ptr, mem_reg->amount, 0);
            if (res != CUDA_SUCCESS) {
                /* If registering the memory fails, print a message and continue.
                 * This is not a fatal error. */
                opal_show_help("help-mpi-common-cuda.txt", "cuMemHostRegister failed",
                               true, mem_reg->ptr, mem_reg->amount,
                               ompi_process_info.nodename, res, mem_reg->msg);
            } else {
                opal_output_verbose(20, mca_common_cuda_output,
                                    "CUDA: cuMemHostRegister OK on mpool %s: "
                                    "address=%p, bufsize=%d",
                                    mem_reg->msg, mem_reg->ptr, (int)mem_reg->amount);
            }
        }
        free(mem_reg->msg);
        OBJ_RELEASE(mem_reg);
    }

    /* Create stream for use in ipc asynchronous copies */
    res = cuFunc.cuStreamCreate(&ipcStream, 0);
    if (res != CUDA_SUCCESS) {
        opal_show_help("help-mpi-common-cuda.txt", "cuStreamCreate failed",
                       true, res);
        return OMPI_ERROR;
    }

    /* Create stream for use in dtoh asynchronous copies */
    res = cuFunc.cuStreamCreate(&dtohStream, 0);
    if (res != CUDA_SUCCESS) {
        opal_show_help("help-mpi-common-cuda.txt", "cuStreamCreate failed",
                       true, res);
        return OMPI_ERROR;

    }

    /* Create stream for use in htod asynchronous copies */
    res = cuFunc.cuStreamCreate(&htodStream, 0);
    if (res != CUDA_SUCCESS) {
        opal_show_help("help-mpi-common-cuda.txt", "cuStreamCreate failed",
                       true, res);
        return OMPI_ERROR;

    }

    opal_output_verbose(30, mca_common_cuda_output,
                        "CUDA: initialized");
    common_cuda_initialized = true;
    return OMPI_SUCCESS;
}

/**
 * This function will open and load the symbols needed from the CUDA driver
 * library.  Any failure will result in a message and we will return 1.
 * First look for the SONAME of the library which is libcuda.so.1.
 */
#define NUMLIBS 2
#define SEARCHPATHS 2
static int mca_common_cuda_load_libcuda(void)
{
    opal_lt_dladvise advise;
    int retval, i, j;
    int advise_support = 1;
    bool loaded = false;
    char *cudalibs[NUMLIBS] = {"libcuda.so.1", "libcuda.so"};
    char *searchpaths[SEARCHPATHS] = {NULL, "/usr/lib64"};
    char **errmsgs = NULL;
    char *errmsg = NULL;
    int errsize;

    if (0 != (retval = opal_lt_dlinit())) {
        if (OPAL_ERR_NOT_SUPPORTED == retval) {
            opal_show_help("help-mpi-common-cuda.txt", "dlopen disabled", true);
        } else {
            opal_show_help("help-mpi-common-cuda.txt", "unknown ltdl error", true,
                           "opal_lt_dlinit", retval, opal_lt_dlerror());
        }
        return 1;
    }

    /* Initialize the lt_dladvise structure.  If this does not work, we can
     * proceed without the support.  Things should still work.  */
    if (0 != (retval = opal_lt_dladvise_init(&advise))) {
        if (OPAL_ERR_NOT_SUPPORTED == retval) {
            advise_support = 0;
        } else {
            opal_show_help("help-mpi-common-cuda.txt", "unknown ltdl error", true,
                           "opal_lt_dladvise_init", retval, opal_lt_dlerror());
            return 1;
        }
    }

    /* Now walk through all the potential names libcuda and find one
     * that works.  If it does, all is good.  If not, print out all
     * the messages about why things failed.  This code was careful
     * to try and save away all error messages if the loading ultimately
     * failed to help with debugging.  
     * NOTE: On the first loop we just utilize the default loading
     * paths from the system.  For the second loop, set /usr/lib64 to
     * the search path and try again.  This is done to handle the case
     * where we have both 32 and 64 bit libcuda.so libraries
     * installed.  If we are running in 64 bit mode, then the loading
     * /usr/lib/libcuda.so will fail and no more searching will be
     * done.  Note that we only set this search path after the
     * original search.  This is so that LD_LIBRARY_PATH and run path
     * settings are respected.  Setting this search path overrides
     * them.  It really should just be appended. */
    if (advise_support) {
        if (0 != (retval = opal_lt_dladvise_global(&advise))) {
            opal_show_help("help-mpi-common-cuda.txt", "unknown ltdl error", true,
                           "opal_lt_dladvise_global", retval, opal_lt_dlerror());
            opal_lt_dladvise_destroy(&advise);
            return 1;
        }
        for (j = 0; j < SEARCHPATHS; j++) {
            if (NULL != searchpaths[j]) {
                opal_lt_dlsetsearchpath(searchpaths[j]);
            }
            for (i = 0; i < NUMLIBS; i++) {
                const char *str;
                libcuda_handle = opal_lt_dlopenadvise(cudalibs[i], advise);
                if (NULL == libcuda_handle) {
                    str = opal_lt_dlerror();
                    if (NULL != str) {
                        opal_argv_append(&errsize, &errmsgs, str);
                    } else {
                        opal_argv_append(&errsize, &errmsgs, "lt_dlerror() returned NULL.");
                    }
                    opal_output_verbose(10, mca_common_cuda_output,
                                        "CUDA: Library open error: %s",
                                        errmsgs[errsize-1]);
                } else {
                    opal_output_verbose(10, mca_common_cuda_output,
                                        "CUDA: Library successfully opened %s",
                                        cudalibs[i]);
                    loaded = true;
                    break;
                }
            }
            if (true == loaded) break; /* Break out of outer loop */
        }
        opal_lt_dladvise_destroy(&advise);
    } else {
        /* No lt_dladvise support.  This should rarely happen. */
        for (j = 0; j < SEARCHPATHS; j++) {
            if (NULL != searchpaths[j]) {
                opal_lt_dlsetsearchpath(searchpaths[j]);
            }
            for (i = 0; i < NUMLIBS; i++) {
                const char *str;
                libcuda_handle = opal_lt_dlopen(cudalibs[i]);
                if (NULL == libcuda_handle) {
                    str = opal_lt_dlerror();
                    if (NULL != str) {
                        opal_argv_append(&errsize, &errmsgs, str);
                    } else {
                        opal_argv_append(&errsize, &errmsgs, "lt_dlerror() returned NULL.");
                    }

                    opal_output_verbose(10, mca_common_cuda_output,
                                        "CUDA: Library open error: %s",
                                        errmsgs[errsize-1]);

                } else {
                    opal_output_verbose(10, mca_common_cuda_output,
                                        "CUDA: Library successfully opened %s",
                                        cudalibs[i]);
                    loaded = true;
                    break;
                }
            }
            if (true == loaded) break; /* Break out of outer loop */
        }
    }

    if (loaded != true) {
        errmsg = opal_argv_join(errmsgs, '\n');
        opal_show_help("help-mpi-common-cuda.txt", "dlopen failed", true,
                       errmsg);
    }
    opal_argv_free(errmsgs);
    free(errmsg);
        
    if (loaded != true) {
        return 1;
    }

    /* Map in the functions that we need */
    OMPI_CUDA_DLSYM(libcuda_handle, cuStreamCreate);
    OMPI_CUDA_DLSYM(libcuda_handle, cuCtxGetCurrent);
    OMPI_CUDA_DLSYM(libcuda_handle, cuEventCreate);
    OMPI_CUDA_DLSYM(libcuda_handle, cuEventRecord);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemHostRegister);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemHostUnregister);
    OMPI_CUDA_DLSYM(libcuda_handle, cuPointerGetAttribute);
    OMPI_CUDA_DLSYM(libcuda_handle, cuEventQuery);
    OMPI_CUDA_DLSYM(libcuda_handle, cuEventDestroy);
    OMPI_CUDA_DLSYM(libcuda_handle, cuStreamWaitEvent);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemcpyAsync);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemcpy);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemFree);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemAlloc);
    OMPI_CUDA_DLSYM(libcuda_handle, cuMemGetAddressRange);
#if OMPI_CUDA_SUPPORT_41
    OMPI_CUDA_DLSYM(libcuda_handle, cuIpcGetEventHandle);
    OMPI_CUDA_DLSYM(libcuda_handle, cuIpcOpenEventHandle);
    OMPI_CUDA_DLSYM(libcuda_handle, cuIpcOpenMemHandle);
    OMPI_CUDA_DLSYM(libcuda_handle, cuIpcCloseMemHandle);
    OMPI_CUDA_DLSYM(libcuda_handle, cuIpcGetMemHandle);
#endif /* OMPI_CUDA_SUPPORT_41 */
    return 0;
}

/**
 * Call the CUDA register function so we pin the memory in the CUDA
 * space.
 */
void mca_common_cuda_register(void *ptr, size_t amount, char *msg) {
    int res;

    if (!common_cuda_initialized) {
        common_cuda_mem_regs_t *regptr;
        if (!common_cuda_init_function_added) {
            opal_cuda_add_initialization_function(&mca_common_cuda_init);
            OBJ_CONSTRUCT(&common_cuda_memory_registrations, opal_list_t);
            common_cuda_init_function_added = true;
        }
        regptr = OBJ_NEW(common_cuda_mem_regs_t);
        regptr->ptr = ptr;
        regptr->amount = amount;
        regptr->msg = strdup(msg);
        opal_list_append(&common_cuda_memory_registrations,
                         (opal_list_item_t*)regptr);
        return;
    }

    if (mca_common_cuda_enabled && mca_common_cuda_register_memory) {
        res = cuFunc.cuMemHostRegister(ptr, amount, 0);
        if (res != CUDA_SUCCESS) {
            /* If registering the memory fails, print a message and continue.
             * This is not a fatal error. */
            opal_show_help("help-mpi-common-cuda.txt", "cuMemHostRegister failed",
                           true, ptr, amount, 
                           ompi_process_info.nodename, res, msg);
        } else {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuMemHostRegister OK on mpool %s: "
                                "address=%p, bufsize=%d",
                                msg, ptr, (int)amount);
        }
    }
}

/**
 * Call the CUDA unregister function so we unpin the memory in the CUDA
 * space.
 */
void mca_common_cuda_unregister(void *ptr, char *msg) {
    int res, i, s;
    common_cuda_mem_regs_t *mem_reg;

    /* This can happen if memory was queued up to be registered, but
     * no CUDA operations happened, so it never was registered.
     * Therefore, just release any of the resources. */
    if (false == common_cuda_initialized) {
        s = opal_list_get_size(&common_cuda_memory_registrations);
        for(i = 0; i < s; i++) {
            mem_reg = (common_cuda_mem_regs_t *)
                opal_list_remove_first(&common_cuda_memory_registrations);
            free(mem_reg->msg);
            OBJ_RELEASE(mem_reg);
        }
        return;
    }

    if (mca_common_cuda_enabled && mca_common_cuda_register_memory) {
        res = cuFunc.cuMemHostUnregister(ptr);
        if (res != CUDA_SUCCESS) {
            /* If unregistering the memory fails, print a message and continue.
             * This is not a fatal error. */
            opal_show_help("help-mpi-common-cuda.txt", "cuMemHostUnregister failed",
                           true, ptr,
                           ompi_process_info.nodename, res, msg);
        } else {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuMemHostUnregister OK on mpool %s: "
                                "address=%p",
                                msg, ptr);
        }
    }
}

#if OMPI_CUDA_SUPPORT_41
/*
 * Get the memory handle of a local section of memory that can be sent
 * to the remote size so it can access the memory.  This is the
 * registration function for the sending side of a message transfer.
 */
int cuda_getmemhandle(void *base, size_t size, mca_mpool_base_registration_t *newreg,
                      mca_mpool_base_registration_t *hdrreg)

{
    CUmemorytype memType;
    CUresult result;
    CUipcMemHandle memHandle;
    CUdeviceptr pbase;
    size_t psize;

    mca_mpool_common_cuda_reg_t *cuda_reg = (mca_mpool_common_cuda_reg_t*)newreg;

    /* We should only be there if this is a CUDA device pointer */
    result = cuFunc.cuPointerGetAttribute(&memType,
                                          CU_POINTER_ATTRIBUTE_MEMORY_TYPE, (CUdeviceptr)base);
    assert(CUDA_SUCCESS == result);
    assert(CU_MEMORYTYPE_DEVICE == memType);

    /* Get the memory handle so we can send it to the remote process. */
    result = cuFunc.cuIpcGetMemHandle(&memHandle, (CUdeviceptr)base);
    CUDA_DUMP_MEMHANDLE((100, &memHandle, "GetMemHandle-After"));

    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuIpcGetMemHandle failed",
                       true, result, base);
        return OMPI_ERROR;
    } else {
        opal_output_verbose(20, mca_common_cuda_output,
                            "CUDA: cuIpcGetMemHandle passed: base=%p size=%d",
                            base, (int)size);
    }

    /* Need to get the real base and size of the memory handle.  This is
     * how the remote side saves the handles in a cache. */
    result = cuFunc.cuMemGetAddressRange(&pbase, &psize, (CUdeviceptr)base);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuMemGetAddressRange failed",
                       true, result, base);
        return OMPI_ERROR;
    } else {
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuMemGetAddressRange passed: addr=%p, size=%d, pbase=%p, psize=%d ",
                            base, (int)size, (void *)pbase, (int)psize);
    }

    /* Store all the information in the registration */
    cuda_reg->base.base = (void *)pbase;
    cuda_reg->base.bound = (unsigned char *)pbase + psize - 1;
    memcpy(&cuda_reg->memHandle, &memHandle, sizeof(memHandle));

    /* Need to record the event to ensure that any memcopies into the
     * device memory have completed.  The event handle associated with
     * this event is sent to the remote process so that it will wait
     * on this event prior to copying data out of the device memory.
     * Note that this needs to be the NULL stream to make since it is
     * unknown what stream any copies into the device memory were done
     * with. */
    result = cuFunc.cuEventRecord((CUevent)cuda_reg->event, 0);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                       true, result, base);
        return OMPI_ERROR;
    }

    return OMPI_SUCCESS;
}

/*
 * This function is called by the local side that called the cuda_getmemhandle.
 * There is nothing to be done so just return.
 */
int cuda_ungetmemhandle(void *reg_data, mca_mpool_base_registration_t *reg) 
{
    CUDA_DUMP_EVTHANDLE((100, ((mca_mpool_common_cuda_reg_t *)reg)->evtHandle, "cuda_ungetmemhandle"));
    opal_output_verbose(10, mca_common_cuda_output,
                        "CUDA: cuda_ungetmemhandle (no-op): base=%p", reg->base);

    return OMPI_SUCCESS;
}

/* 
 * Open a memory handle that refers to remote memory so we can get an address
 * that works on the local side.  This is the registration function for the
 * remote side of a transfer.  newreg contains the new handle.  hddrreg contains
 * the memory handle that was received from the remote side.
 */
int cuda_openmemhandle(void *base, size_t size, mca_mpool_base_registration_t *newreg,
                       mca_mpool_base_registration_t *hdrreg)
{
    CUresult result;
    CUipcMemHandle memHandle;
    mca_mpool_common_cuda_reg_t *cuda_newreg = (mca_mpool_common_cuda_reg_t*)newreg;

    /* Need to copy into memory handle for call into CUDA library. */
    memcpy(&memHandle, cuda_newreg->memHandle, sizeof(memHandle));
    CUDA_DUMP_MEMHANDLE((100, &memHandle, "Before call to cuIpcOpenMemHandle"));

    /* Open the memory handle and store it into the registration structure. */
    result = cuFunc.cuIpcOpenMemHandle((CUdeviceptr *)&newreg->alloc_base, memHandle,
                                       CU_IPC_MEM_LAZY_ENABLE_PEER_ACCESS);

    /* If there are some stale entries in the cache, they can cause other
     * registrations to fail.  Let the caller know that so that can attempt
     * to clear them out. */
    if (CUDA_ERROR_ALREADY_MAPPED == result) {
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuIpcOpenMemHandle returned CUDA_ERROR_ALREADY_MAPPED for "
                            "p=%p,size=%d: notify memory pool\n", base, (int)size);
        return OMPI_ERR_WOULD_BLOCK;
    }
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuIpcOpenMemHandle failed",
                       true, result, base);
        /* Currently, this is a non-recoverable error */
        return OMPI_ERROR;
    } else {
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuIpcOpenMemHandle passed: base=%p (remote base=%p,size=%d)",
                            newreg->alloc_base, base, (int)size);
        CUDA_DUMP_MEMHANDLE((200, &memHandle, "cuIpcOpenMemHandle"));
    }

    return OMPI_SUCCESS;
}

/* 
 * Close a memory handle that refers to remote memory. 
 */
int cuda_closememhandle(void *reg_data, mca_mpool_base_registration_t *reg)
{
    CUresult result;
    mca_mpool_common_cuda_reg_t *cuda_reg = (mca_mpool_common_cuda_reg_t*)reg;

    result = cuFunc.cuIpcCloseMemHandle((CUdeviceptr)cuda_reg->base.alloc_base);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuIpcCloseMemHandle failed",
                       true, result, cuda_reg->base.alloc_base);
        /* We will just continue on and hope things continue to work. */
    } else {
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuIpcCloseMemHandle passed: base=%p",
                            cuda_reg->base.alloc_base);
        CUDA_DUMP_MEMHANDLE((100, cuda_reg->memHandle, "cuIpcCloseMemHandle"));
    }

    return OMPI_SUCCESS;
}

void mca_common_cuda_construct_event_and_handle(uint64_t **event, void **handle)
{
    CUresult result;

    result = cuFunc.cuEventCreate((CUevent *)event, CU_EVENT_INTERPROCESS | CU_EVENT_DISABLE_TIMING);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventCreate failed",
                       true, result);
    }

    result = cuFunc.cuIpcGetEventHandle((CUipcEventHandle *)handle, (CUevent)*event);
    if (CUDA_SUCCESS != result){
        opal_show_help("help-mpi-common-cuda.txt", "cuIpcGetEventHandle failed",
                       true, result);
    }

    CUDA_DUMP_EVTHANDLE((10, handle, "construct_event_and_handle"));

}

void mca_common_cuda_destruct_event(uint64_t *event)
{
    CUresult result;

    result = cuFunc.cuEventDestroy((CUevent)event);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventDestroy failed",
                       true, result);
    }
}


/*
 * Put remote event on stream to ensure that the the start of the
 * copy does not start until the completion of the event.
 */
void mca_common_wait_stream_synchronize(mca_mpool_common_cuda_reg_t *rget_reg)
{
    CUipcEventHandle evtHandle;
    CUevent event;
    CUresult result;

    memcpy(&evtHandle, rget_reg->evtHandle, sizeof(evtHandle));
    CUDA_DUMP_EVTHANDLE((100, &evtHandle, "stream_synchronize"));

    result = cuFunc.cuIpcOpenEventHandle(&event, evtHandle);
    if (CUDA_SUCCESS != result){
        opal_show_help("help-mpi-common-cuda.txt", "cuIpcOpenEventHandle failed",
                       true, result);
    }

    /* BEGIN of Workaround - There is a bug in CUDA 4.1 RC2 and earlier
     * versions.  Need to record an event on the stream, even though
     * it is not used, to make sure we do not short circuit our way
     * out of the cuStreamWaitEvent test.
     */
    result = cuFunc.cuEventRecord(event, 0);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                       true, result);
    }
    /* END of Workaround */

    result = cuFunc.cuStreamWaitEvent(0, event, 0);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuStreamWaitEvent failed",
                       true, result);
    }

    /* All done with this event. */
    result = cuFunc.cuEventDestroy(event);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventDestroy failed",
                       true, result);
    }
}

/*
 * Start the asynchronous copy.  Then record and save away an event that will
 * be queried to indicate the copy has completed.
 */
int mca_common_cuda_memcpy(void *dst, void *src, size_t amount, char *msg, 
                           struct mca_btl_base_descriptor_t *frag, int *done)
{
    CUresult result;
    int iter;

    /* First make sure there is room to store the event.  If not, then
     * return an error.  The error message will tell the user to try and
     * run again, but with a larger array for storing events. */
    if (cuda_event_ipc_num_used == cuda_event_max) {
        opal_show_help("help-mpi-common-cuda.txt", "Out of cuEvent handles",
                       true, cuda_event_max, cuda_event_max+100, cuda_event_max+100);
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    /* This is the standard way to run.  Running with synchronous copies is available
     * to measure the advantages of asynchronous copies. */
    if (OPAL_LIKELY(mca_common_cuda_async)) {
        result = cuFunc.cuMemcpyAsync((CUdeviceptr)dst, (CUdeviceptr)src, amount, ipcStream);
        if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuMemcpyAsync failed",
                           true, dst, src, amount, result);
            return OMPI_ERROR;
        } else {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuMemcpyAsync passed: dst=%p, src=%p, size=%d",
                                dst, src, (int)amount);
        }
        result = cuFunc.cuEventRecord(cuda_event_ipc_array[cuda_event_ipc_first_avail], ipcStream);
        if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                           true, result);
            return OMPI_ERROR;
        }
        cuda_event_ipc_frag_array[cuda_event_ipc_first_avail] = frag;

        /* Bump up the first available slot and number used by 1 */
        cuda_event_ipc_first_avail++;
        if (cuda_event_ipc_first_avail >= cuda_event_max) {
            cuda_event_ipc_first_avail = 0;
        }
        cuda_event_ipc_num_used++;

        *done = 0;
    } else {
        /* Mimic the async function so they use the same memcpy call. */
        result = cuFunc.cuMemcpyAsync((CUdeviceptr)dst, (CUdeviceptr)src, amount, ipcStream);
        if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuMemcpyAsync failed",
                           true, dst, src, amount, result);
            return OMPI_ERROR;
        } else {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuMemcpyAsync passed: dst=%p, src=%p, size=%d",
                                dst, src, (int)amount);
        }

        /* Record an event, then wait for it to complete with calls to cuEventQuery */
        result = cuFunc.cuEventRecord(cuda_event_ipc_array[cuda_event_ipc_first_avail], ipcStream);
        if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                           true, result);
            return OMPI_ERROR;
        }

        cuda_event_ipc_frag_array[cuda_event_ipc_first_avail] = frag;

        /* Bump up the first available slot and number used by 1 */
        cuda_event_ipc_first_avail++;
        if (cuda_event_ipc_first_avail >= cuda_event_max) {
            cuda_event_ipc_first_avail = 0;
        }
        cuda_event_ipc_num_used++;

        result = cuFunc.cuEventQuery(cuda_event_ipc_array[cuda_event_ipc_first_used]);
        if ((CUDA_SUCCESS != result) && (CUDA_ERROR_NOT_READY != result)) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventQuery failed",
                           true, result);
            return OMPI_ERROR;
        }

        iter = 0;
        while (CUDA_ERROR_NOT_READY == result) {
            if (0 == (iter % 10)) {
                opal_output(-1, "EVENT NOT DONE (iter=%d)", iter);
            }
            result = cuFunc.cuEventQuery(cuda_event_ipc_array[cuda_event_ipc_first_used]);
            if ((CUDA_SUCCESS != result) && (CUDA_ERROR_NOT_READY != result)) {
                opal_show_help("help-mpi-common-cuda.txt", "cuEventQuery failed",
                               true, result);
                return OMPI_ERROR;
            }
            iter++;
        }

        --cuda_event_ipc_num_used;
        ++cuda_event_ipc_first_used;
        if (cuda_event_ipc_first_used >= cuda_event_max) {
            cuda_event_ipc_first_used = 0;
        }
        *done = 1;
    }  
    return OMPI_SUCCESS;
}

/*
 * Record an event and save the frag.  This is called by the sending side and
 * is used to queue an event when a htod copy has been initiated.
 */
int mca_common_cuda_record_dtoh_event(char *msg, struct mca_btl_base_descriptor_t *frag)
{
    CUresult result;

    /* First make sure there is room to store the event.  If not, then
     * return an error.  The error message will tell the user to try and
     * run again, but with a larger array for storing events. */
    if (cuda_event_dtoh_num_used == cuda_event_max) {
        opal_show_help("help-mpi-common-cuda.txt", "Out of cuEvent handles",
                       true, cuda_event_max, cuda_event_max+100, cuda_event_max+100);
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    result = cuFunc.cuEventRecord(cuda_event_dtoh_array[cuda_event_dtoh_first_avail], dtohStream);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                       true, result);
        return OMPI_ERROR;
    }
    cuda_event_dtoh_frag_array[cuda_event_dtoh_first_avail] = frag;

    /* Bump up the first available slot and number used by 1 */
    cuda_event_dtoh_first_avail++;
    if (cuda_event_dtoh_first_avail >= cuda_event_max) {
        cuda_event_dtoh_first_avail = 0;
    }
    cuda_event_dtoh_num_used++;

    return OMPI_SUCCESS;
}

/*
 * Record an event and save the frag.  This is called by the receiving side and
 * is used to queue an event when a dtoh copy has been initiated.
 */
int mca_common_cuda_record_htod_event(char *msg, struct mca_btl_base_descriptor_t *frag)
{
    CUresult result;

    /* First make sure there is room to store the event.  If not, then
     * return an error.  The error message will tell the user to try and
     * run again, but with a larger array for storing events. */
    if (cuda_event_htod_num_used == cuda_event_max) {
        opal_show_help("help-mpi-common-cuda.txt", "Out of cuEvent handles",
                       true, cuda_event_max, cuda_event_max+100, cuda_event_max+100);
        return OMPI_ERR_OUT_OF_RESOURCE;
    }

    result = cuFunc.cuEventRecord(cuda_event_htod_array[cuda_event_htod_first_avail], htodStream);
    if (CUDA_SUCCESS != result) {
        opal_show_help("help-mpi-common-cuda.txt", "cuEventRecord failed",
                       true, result);
        return OMPI_ERROR;
    }
    cuda_event_htod_frag_array[cuda_event_htod_first_avail] = frag;
 
   /* Bump up the first available slot and number used by 1 */
    cuda_event_htod_first_avail++;
    if (cuda_event_htod_first_avail >= cuda_event_max) {
        cuda_event_htod_first_avail = 0;
    }
    cuda_event_htod_num_used++;

    return OMPI_SUCCESS;
}

/**
 * Used to get the dtoh stream for initiating asynchronous copies.
 */
void *mca_common_cuda_get_dtoh_stream(void) {
    return (void *)dtohStream;
}

/**
 * Used to get the htod stream for initiating asynchronous copies.
 */
void *mca_common_cuda_get_htod_stream(void) {
    return (void *)htodStream;
}

/*
 * Function is called every time progress is called with the sm BTL.  If there
 * are outstanding events, check to see if one has completed.  If so, hand
 * back the fragment for further processing.
 */
int progress_one_cuda_ipc_event(struct mca_btl_base_descriptor_t **frag) {
    CUresult result;

    if (cuda_event_ipc_num_used > 0) {
        opal_output_verbose(20, mca_common_cuda_output,
                           "CUDA: progress_one_cuda_ipc_event, outstanding_events=%d",
                            cuda_event_ipc_num_used);

        result = cuFunc.cuEventQuery(cuda_event_ipc_array[cuda_event_ipc_first_used]);

        /* We found an event that is not ready, so return. */
        if (CUDA_ERROR_NOT_READY == result) {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuEventQuery returned CUDA_ERROR_NOT_READY");
            *frag = NULL;
            return 0;
        } else if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventQuery failed",
                           true, result);
            *frag = NULL;
            return OMPI_ERROR;
        }

        *frag = cuda_event_ipc_frag_array[cuda_event_ipc_first_used];
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuEventQuery returned %d", result);

        /* Bump counters, loop around the circular buffer if necessary */
        --cuda_event_ipc_num_used;
        ++cuda_event_ipc_first_used;
        if (cuda_event_ipc_first_used >= cuda_event_max) {
            cuda_event_ipc_first_used = 0;
        }
        /* A return value of 1 indicates an event completed and a frag was returned */
        return 1;
    }
    return 0;
}

/**
 * Progress any dtoh event completions.
 */
int progress_one_cuda_dtoh_event(struct mca_btl_base_descriptor_t **frag) {
    CUresult result;

    if (cuda_event_dtoh_num_used > 0) {
        opal_output_verbose(20, mca_common_cuda_output,
                           "CUDA: progress_one_cuda_dtoh_event, outstanding_events=%d",
                            cuda_event_dtoh_num_used);

        result = cuFunc.cuEventQuery(cuda_event_dtoh_array[cuda_event_dtoh_first_used]);

        /* We found an event that is not ready, so return. */
        if (CUDA_ERROR_NOT_READY == result) {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuEventQuery returned CUDA_ERROR_NOT_READY");
            *frag = NULL;
            return 0;
        } else if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventQuery failed",
                           true, result);
            *frag = NULL;
            return OMPI_ERROR;
        }

        *frag = cuda_event_dtoh_frag_array[cuda_event_dtoh_first_used];
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuEventQuery returned %d", result);

        /* Bump counters, loop around the circular buffer if necessary */
        --cuda_event_dtoh_num_used;
        ++cuda_event_dtoh_first_used;
        if (cuda_event_dtoh_first_used >= cuda_event_max) {
            cuda_event_dtoh_first_used = 0;
        }
        /* A return value of 1 indicates an event completed and a frag was returned */
        return 1;
    }
    return 0;
}

/**
 * Progress any dtoh event completions.
 */
int progress_one_cuda_htod_event(struct mca_btl_base_descriptor_t **frag) {
    CUresult result;

    if (cuda_event_htod_num_used > 0) {
        opal_output_verbose(20, mca_common_cuda_output,
                           "CUDA: progress_one_cuda_htod_event, outstanding_events=%d",
                            cuda_event_htod_num_used);

        result = cuFunc.cuEventQuery(cuda_event_htod_array[cuda_event_htod_first_used]);

        /* We found an event that is not ready, so return. */
        if (CUDA_ERROR_NOT_READY == result) {
            opal_output_verbose(20, mca_common_cuda_output,
                                "CUDA: cuEventQuery returned CUDA_ERROR_NOT_READY");
            *frag = NULL;
            return 0;
        } else if (CUDA_SUCCESS != result) {
            opal_show_help("help-mpi-common-cuda.txt", "cuEventQuery failed",
                           true, result);
            *frag = NULL;
            return OMPI_ERROR;
        }

        *frag = cuda_event_htod_frag_array[cuda_event_htod_first_used];
        opal_output_verbose(10, mca_common_cuda_output,
                            "CUDA: cuEventQuery returned %d", result);

        /* Bump counters, loop around the circular buffer if necessary */
        --cuda_event_htod_num_used;
        ++cuda_event_htod_first_used;
        if (cuda_event_htod_first_used >= cuda_event_max) {
            cuda_event_htod_first_used = 0;
        }
        /* A return value of 1 indicates an event completed and a frag was returned */
        return 1;
    }
    return 0;
}


/**
 * Need to make sure the handle we are retrieving from the cache is still
 * valid.  Compare the cached handle to the one received. 
 */
int mca_common_cuda_memhandle_matches(mca_mpool_common_cuda_reg_t *new_reg, 
                                      mca_mpool_common_cuda_reg_t *old_reg)
{

    if (0 == memcmp(new_reg->memHandle, old_reg->memHandle, sizeof(new_reg->memHandle))) {
        return 1;
    } else {
        return 0;
    }
            
}

/*
 * Function to dump memory handle information.  This is based on 
 * definitions from cuiinterprocess_private.h.
 */
static void cuda_dump_memhandle(int verbose, void *memHandle, char *str) {

    struct InterprocessMemHandleInternal 
    {
        /* The first two entries are the CUinterprocessCtxHandle */
        int64_t ctxId; /* unique (within a process) id of the sharing context */
        int     pid;   /* pid of sharing context */

        int64_t size;
        int64_t blocksize;
        int64_t offset;
        int     gpuId;
        int     subDeviceIndex;
        int64_t serial;
    } memH;

    if (NULL == str) {
        str = "CUDA";
    }
    memcpy(&memH, memHandle, sizeof(memH));
    opal_output_verbose(verbose, mca_common_cuda_output,
                        "%s:ctxId=%d, pid=%d, size=%d, blocksize=%d, offset=%d, gpuId=%d, "
                        "subDeviceIndex=%d, serial=%d",
                        str, (int)memH.ctxId, memH.pid, (int)memH.size, (int)memH.blocksize, (int)memH.offset,
                        memH.gpuId, memH.subDeviceIndex, (int)memH.serial);
}

/*
 * Function to dump memory handle information.  This is based on 
 * definitions from cuiinterprocess_private.h.
 */
static void cuda_dump_evthandle(int verbose, void *evtHandle, char *str) {

    struct InterprocessEventHandleInternal 
    {
        /* The first two entries are the CUinterprocessCtxHandle */
        int64_t ctxId; /* unique (within a process) id of the sharing context */
        int     pid;   /* pid of sharing context */

        int     pad;   /* pad to match the structure */
        int     index;
    } evtH;

    if (NULL == str) {
        str = "CUDA";
    }
    memcpy(&evtH, evtHandle, sizeof(evtH));
    opal_output_verbose(verbose, mca_common_cuda_output,
                        "CUDA: %s:ctxId=%d, pid=%d, index=%d",
                        str, (int)evtH.ctxId, evtH.pid, (int)evtH.index);
}


/* Return microseconds of elapsed time. Microseconds are relevant when
 * trying to understand the fixed overhead of the communication. Used 
 * when trying to time various functions.
 *
 * Cut and past the following to get timings where wanted.
 * 
 *   clock_gettime(CLOCK_MONOTONIC, &ts_start);
 *   FUNCTION OF INTEREST
 *   clock_gettime(CLOCK_MONOTONIC, &ts_end);
 *   accum = mydifftime(ts_start, ts_end);
 *   opal_output(0, "Function took   %7.2f usecs\n", accum);
 *
 */
#if CUDA_COMMON_TIMING
static float mydifftime(struct timespec ts_start, struct timespec ts_end) {
    float seconds;
    float microseconds;
    float nanoseconds;

    /* If we did not rollover the seconds clock, then we just take
     * the difference between the nanoseconds clock for actual time */
    if (0 == (ts_end.tv_sec - ts_start.tv_sec)) {
        nanoseconds = (float)(ts_end.tv_nsec - ts_start.tv_nsec);
        return nanoseconds / THOUSAND;
    } else {
        seconds = (float)(ts_end.tv_sec - ts_start.tv_sec); 

        /* Note that this value can be negative or positive
         * which is fine.  In the case that it is negative, it
         * just gets subtracted from the difference which is what
         * we want. */
        nanoseconds = (float)(ts_end.tv_nsec - ts_start.tv_nsec);
        microseconds = (seconds * MILLION) + (nanoseconds/THOUSAND);
        return microseconds;
    }
}
#endif /* CUDA_COMMON_TIMING */

#endif /* OMPI_CUDA_SUPPORT_41 */

/* Routines that get plugged into the opal datatype code */
static int mca_common_cuda_is_gpu_buffer(const void *pUserBuf)
{
    int res;
    CUmemorytype memType;
    CUdeviceptr dbuf = (CUdeviceptr)pUserBuf;

    res = cuFunc.cuPointerGetAttribute(&memType,
                                       CU_POINTER_ATTRIBUTE_MEMORY_TYPE, dbuf);
    if (res != CUDA_SUCCESS) {
        /* If we cannot determine it is device pointer,
         * just assume it is not. */
        return 0;
    } else if (memType == CU_MEMORYTYPE_HOST) {
        /* Host memory, nothing to do here */
        return 0;
    }
    /* Must be a device pointer */
    assert(memType == CU_MEMORYTYPE_DEVICE);
    return 1;
}

static int mca_common_cuda_cu_memcpy_async(void *dest, const void *src, size_t size,
                                         opal_convertor_t* convertor)
{
    return cuFunc.cuMemcpyAsync((CUdeviceptr)dest, (CUdeviceptr)src, size, 
                                (CUstream)convertor->stream);
}

static int mca_common_cuda_cu_memcpy(void *dest, const void *src, size_t size)
{
    return cuFunc.cuMemcpy((CUdeviceptr)dest, (CUdeviceptr)src, size);
}

static int mca_common_cuda_memmove(void *dest, void *src, size_t size)
{
    CUdeviceptr tmp;
    int res;

    res = cuFunc.cuMemAlloc(&tmp,size);
    res = cuFunc.cuMemcpy(tmp, (CUdeviceptr)src, size);
    if(res != CUDA_SUCCESS){
        opal_output(0, "CUDA: memmove-Error in cuMemcpy: res=%d, dest=%p, src=%p, size=%d",
                    res, (void *)tmp, src, (int)size);
        return res;
    }
    res = cuFunc.cuMemcpy((CUdeviceptr)dest, tmp, size);
    if(res != CUDA_SUCCESS){
        opal_output(0, "CUDA: memmove-Error in cuMemcpy: res=%d, dest=%p, src=%p, size=%d",
                    res, dest, (void *)tmp, (int)size);
        return res;
    }
    cuFunc.cuMemFree(tmp);
    return 0;
}
