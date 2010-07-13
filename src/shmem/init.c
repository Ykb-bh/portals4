/* The API definition */
#include <portals4.h>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* System libraries */
#include <assert.h>
#include <stddef.h>		       /* for NULL */
#include <sys/mman.h>		       /* for mmap() and shm_open() */
#include <sys/stat.h>		       /* for S_IRUSR and friends */
#include <fcntl.h>		       /* for O_RDWR */
#include <stdlib.h>		       /* for getenv() */
#include <unistd.h>		       /* for close() */
#include <limits.h>		       /* for UINT_MAX */
#include <string.h>		       /* for memset() */

/* Internals */
#include "ptl_visibility.h"
#include "ptl_internal_atomic.h"
#include "ptl_internal_commpad.h"
#include "ptl_internal_nit.h"

volatile char *comm_pad = NULL;
size_t num_siblings = 0;
size_t proc_number = 0;
size_t per_proc_comm_buf_size = 0;
size_t firstpagesize = 0;

const ptl_pid_t PTL_PID_ANY = UINT_MAX;

static unsigned int init_ref_count = 0;
static size_t comm_pad_size = 0;
static const char *comm_pad_shm_name = NULL;

/* The trick to this function is making it thread-safe: multiple threads can
 * all call PtlInit concurrently, and all will wait until initialization is
 * complete, and if there is a failure, all will report failure.
 *
 * PtlInit() will only work if the process has been executed by yod (which
 * handles important aspects of the init/cleanup and passes data via
 * envariables).
 */
int API_FUNC PtlInit(
    void)
{
    unsigned int race = PtlInternalAtomicInc(&init_ref_count, 1);
    static volatile int done_initializing = 0;
    static volatile int failure = 0;

    if (race == 0) {
	int shm_fd;
	char *strerr = NULL;
	const char *str = NULL;

#ifdef _SC_PAGESIZE
	firstpagesize = sysconf(_SC_PAGESIZE);
#elif defined(_SC_PAGE_SIZE)
	firstpagesize = sysconf(_SC_PAGE_SIZE);
#elif defined(HAVE_GETPAGESIZE)
	firstpagesize = getpagesize();
#else
	firstpagesize = 4096;
#endif

	/* Parse the official yod-provided environment variables */
	comm_pad_shm_name = getenv("PORTALS4_SHM_NAME");
	if (comm_pad_shm_name == NULL) {
	    goto exit_fail;
	}
	str = getenv("PORTALS4_NUM_PROCS");
	if (str == NULL) {
	    goto exit_fail;
	}
	num_siblings = strtol(str, &strerr, 10);
	if (strerr == NULL || strerr == str) {
	    /* could not parse! */
	    goto exit_fail;
	}
	str = getenv("PORTALS4_RANK");
	if (str == NULL) {
	    goto exit_fail;
	}
	proc_number = strtol(str, &strerr, 10);
	if (strerr == NULL || strerr == str) {
	    /* could not parse! */
	    goto exit_fail;
	}
	str = getenv("PORTALS4_COMM_SIZE");
	if (str == NULL) {
	    goto exit_fail;
	}
	per_proc_comm_buf_size = strtol(str, &strerr, 10);
	if (strerr == NULL || strerr == str) {
	    /* could not parse! */
	    goto exit_fail;
	}
	comm_pad_size =
	    firstpagesize + (per_proc_comm_buf_size * num_siblings);

	memset(&nit, 0, sizeof(ptl_internal_nit_t));
	nit_limits.max_mes = INT_MAX;  // more important when using pooling
	nit_limits.max_mds = INT_MAX;  // more important when using pooling
	nit_limits.max_cts = INT_MAX;  // more important when using pooling
	nit_limits.max_eqs = INT_MAX;  // more important when using pooling
	nit_limits.max_pt_index = 63;
	nit_limits.max_iovecs = INT_MAX;	// ???
	nit_limits.max_me_list = nit_limits.max_mes;	// may be smaller if not using a linked-list implementaiton
	nit_limits.max_msg_size = per_proc_comm_buf_size;	// may need to be smaller
	nit_limits.max_atomic_size = 8;	// XXX: does not apply to all architectures

	/* Open the communication pad */
	assert(comm_pad == NULL);
	shm_fd = shm_open(comm_pad_shm_name, O_RDWR, S_IRUSR | S_IWUSR);
	assert(shm_fd >= 0);
	if (shm_fd < 0) {
	    //perror("PtlInit: shm_open failed");
	    goto exit_fail;
	}
	comm_pad =
	    (char *)mmap(NULL, comm_pad_size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, shm_fd, 0);
	if (comm_pad == MAP_FAILED) {
	    //perror("PtlInit: mmap failed");
	    goto exit_fail;
	}
	assert(close(shm_fd) == 0);

	/**************************************************************************
	 * Can Now Announce My Presence
	 **************************************************************************/
	comm_pad[proc_number] = 1;

	/* Now, wait for my siblings to get here. */
	size_t i;
	for (i = 0; i < num_siblings; ++i) {
	    /* oddly enough, this should reduce cache traffic for large numbers
	     * of siblings */
	    while (comm_pad[i] == 0) ;
	}

	/* Release any concurrent initialization calls */
	__sync_synchronize();
	done_initializing = 1;
	return PTL_OK;
    } else {
	/* Should block until other inits finish. */
	while (!done_initializing) ;
	if (!failure)
	    return PTL_OK;
	else
	    goto exit_fail_fast;
    }
  exit_fail:
    failure = 1;
    __sync_synchronize();
    done_initializing = 1;
  exit_fail_fast:
    PtlInternalAtomicInc(&init_ref_count, -1);
    return PTL_FAIL;
}

void API_FUNC PtlFini(
    void)
{
    unsigned int lastone;
    assert(init_ref_count > 0);
    if (init_ref_count == 0)
	return;
    lastone = PtlInternalAtomicInc(&init_ref_count, -1);
    if (lastone == 1) {
	/* Clean up */
	assert(munmap((void *)comm_pad, comm_pad_size) == 0);
	comm_pad = NULL;
    }
}
