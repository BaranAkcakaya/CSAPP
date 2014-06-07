/*
This version used the segregated free list
Implementation Details
0. Possible Maximum allocate size: 32GB
1. Free Block Organization: use segregated free list
2. Coalescing: use immediate coalescing with boundary tags
3. Placement: first-fit
4. Splitting: Splitting only if the size of the reminder would equal or exceed the minimum block size
5. Heap Structure
   [ 1 word padding | block 0 | block 1 | ... ]
6. Block Structure:
   - allocated block: [1 word Header | ... payload ... | optional padding ]
   - free block: [ 1 word header] | 1 word prev_pointer | 1 word next_pointer | .... | 1 word footer ]
7. Minimum block size: 16 bytes
8. Header Structure:
   - [31 - 3] bit for size
   - [2] not used
   - [0] alloc        : indicate whether or current block is allocated
9. Size classes: power of 2
   - [16 ~ 31]
   - [32 ~ 63]
   - ...
   - [2^31 ~ (2^32)-1]
   - total: 28 classes, so we need a static global void* array of size 28
10. How to order free block in each list ?
   - choice 1 ordering by address
   - choice 2 LIFO
11. Analysis
   - malloc is linear to the size of one size class, worse case O(N)
   - free is linear to the size of one size class, if choose ordering by address, otherwise constant time
*/
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
#include "mm.h"
#include "memlib.h"

// Control Marcos
// #define __HEAP_CHECK__
// #define __LOG_TO_STDERR__
// #define __LIFO_ORDERING__
#ifdef __LOG_TO_STDERR__
#define DebugStr(args...)   fprintf(stderr, args);
#else
#define DebugStr(args...)
#endif

#ifdef __HEAP_CHECK__
#include <string.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#endif

team_t team = {
    /* Team name */
    "CSAPP",
    /* First member's full name */
    "CSAPP",
    /* First member's email address */
    "CSAPP@CSAPP.com",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""
};

#ifdef __HEAP_CHECK__
typedef void handler_t(int);

// record the allocated block only
typedef struct HeapStruct {
  char *bk_head;  // block head
  char *bk_tail;  // block tail
  char *pl_head;  // payload head
  char *pl_tail;  // payload tail, pl_tail is possible to equal to bk_tail
  size_t bk_size; // block size
  size_t pl_size; // payload size;
  int index;
  struct HeapStruct *next;
} HeapStruct;

static HeapStruct *alloc_list = NULL;
static HeapStruct *free_list = NULL;

static int writable = 1;
static void* heap_head = NULL; // points to heap start
static void* heap_tail = NULL; // points to heap end, one byte after Epilogue header

void add_to_alloc_list(const void *ptr, const size_t pl_size, const size_t bk_size);
void delete_from_alloc_list(const void *ptr);
HeapStruct* delete_from_list(HeapStruct *list, const void *ptr);
HeapStruct* search_list(const HeapStruct *list, const void *ptr);
int addr_is_allocated(const char *addr);
int addr_is_payload(const char *addr);
int within_heap(const void *addr);
void show_heap(void);
void show_alloc_list(void);
handler_t *Signal(int signum, handler_t *handler);
void print_stack_trace(int signum);
void to_hex_str(size_t num, int sep);
void to_binary_str(size_t num, int sep);
#endif

#include "mm_macros.h"

static void* heap_listp = NULL;
size_t index_array[] = {
  1 << 4, 1 << 5, 1 << 6, 1 << 7, 1 << 8, 1 << 9, 1 << 10,
  1 << 11, 1 << 12, 1 << 13, 1 << 14, 1 << 15, 1 << 16, 1 << 17,
  1 << 18, 1 << 19, 1 << 20, 1 << 21, 1 << 22, 1 << 23, 1 << 24,
  1 << 25, 1 << 26, 1 << 27, 1 << 28, 1 << 29, 1 << 30, 1 << 31
};

void *segregated_free_list[sizeof(index_array) / sizeof(index_array[0])];
int index_arr_size = sizeof(index_array) / sizeof(index_array[0]);

void *extend_heap(size_t size);
void *coalesce(void *pldp);
void *find_first_fit(size_t asize);
void place_and_split(void *pldp, size_t asize);
void *forward_collect(void *hdrp, size_t *collected_size, size_t target_size);
void *backward_collect(void *hdrp, size_t target_size);
void mm_memcpy(void *dst, void *src, size_t num);

// helper functions
int find_index(size_t size);
void init_free_block(void *hdrp, size_t bk_size);
void insert_into_size_class(void *hdrp, int index);
void print_list(void *hdrp);

/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void) {
#ifdef __HEAP_CHECK__
  Signal(SIGABRT, print_stack_trace);
  heap_head = NULL;
  heap_tail = NULL;
  alloc_list = NULL;
#endif

  // 1 WSIZE for heap-start padding
  // 1 WSIZE for heap-end padding
  if ((heap_listp = mem_sbrk(2 * WSIZE)) == (void*)(-1)) {
    return -1;
  }

#ifdef __HEAP_CHECK__
  heap_head = heap_listp;
  heap_tail = heap_listp + 2 * WSIZE;
#endif

  WRITE_WORD(heap_listp, PACK(0, CURR_ALLOC));  // heap-start 0 padding
  WRITE_WORD(heap_listp + WSIZE, PACK(0, CURR_ALLOC));  // heap-end 0 padding
  heap_listp += WSIZE;

  int i = 0;
  for (i = 0; i < index_arr_size; ++i) {
    segregated_free_list[i] = NULL;
  }

  return extend_heap(CHUNKSIZE) == NULL ? -1 : 0;
}

/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *mm_malloc(size_t size) {
  // Ignore spurious requests
  if (!size) return NULL;

  void *ret = NULL;
  size_t asize = ALIGN(size + WSIZE);

  void *pldp = find_first_fit(asize);
  if (pldp) {

#ifdef __HEAP_CHECK__
    assert(within_heap(pldp));
    assert(!addr_is_allocated(pldp)); // lies with free space
    assert(!addr_is_payload(pldp));
#endif
    place_and_split(pldp, asize);
    ret = pldp;
  } else if ((pldp = extend_heap(asize))) {
    place_and_split(pldp, asize);
    ret = pldp;
  }

#ifdef __HEAP_CHECK__
  add_to_alloc_list(ret, size, asize);
#endif
  return ret;
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr) {
#ifdef __HEAP_CHECK__
  delete_from_alloc_list(ptr);
#endif
  assert(NULL);
}

/*
 * mm_realloc - Implemented simply in terms of mm_malloc and mm_free
 */
void *mm_realloc(void *ptr, size_t size) { assert(NULL); }

int find_index(size_t size) {
  int low = 0, high = index_arr_size;
  int ret = -1;
  int mid;
  while (low < high) {
    mid = (high + low) >> 1;
    if (size < index_array[mid]) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
  return low - 1;
}

void *extend_heap(size_t size) {
  char *hdrp = NULL;
  size_t asize = ALIGN_CHUNKSIZE(size);
  if ((void*)(hdrp = mem_sbrk(asize)) == (void*)(-1)) return NULL;

#ifdef __HEAP_CHECK__
  assert(hdrp == heap_tail);
  assert(!addr_is_allocated(hdrp));
  assert(!addr_is_allocated((char*)hdrp + asize));
  heap_tail += asize;
#endif

  hdrp = (char*)hdrp - WSIZE; // points to heap-end 0 padding
  init_free_block(hdrp, asize); // pack header and footer
  insert_into_size_class(hdrp, find_index(asize));

#ifdef __HEAP_CHECK__
  assert((hdrp + asize) == ((char*)heap_tail - WSIZE));
#endif

  WRITE_WORD(hdrp + asize, PACK(0, CURR_ALLOC)); // write new heap-end 0 padding

  return coalesce((char*)hdrp + WSIZE);
}

void *coalesce(void *pldp) {
  void *hdrp = HDRP_USE_PLDP(pldp);
  size_t size = GET_SIZE(hdrp);
  size_t prev_alloc = GET_PREV_ALLOC(hdrp); // read current header to get the privous block allocate bit
  size_t next_alloc = GET_ALLOC((char*)hdrp + size); // read next block header to get its allocate bit

  // previous and next are both allocated
  if (prev_alloc && next_alloc) {
    return pldp;
  }

  // previous is free but next is allocated
  if (!prev_alloc && next_alloc) {
    void *prev_hdrp = (char*)hdrp - GET_SIZE((char*)hdrp - WSIZE); // get previous block header pointer
    WRITE_WORD(prev_hdrp, READ_WORD(prev_hdrp) + size); // write new header in previous block
    WRITE_WORD((char*)hdrp + size - WSIZE, READ_WORD(prev_hdrp)); // write new footer in current block
    return (char*)prev_hdrp + WSIZE;
  }

  // next is free but previous is allocated
  if (prev_alloc && !next_alloc) {
    void *next_hdrp = (char*)hdrp + size;
    size_t next_size = GET_SIZE(next_hdrp);
    WRITE_WORD(hdrp, READ_WORD(hdrp) + next_size); // write new header in current block
    WRITE_WORD((char*)next_hdrp + next_size - WSIZE, READ_WORD(hdrp)); // write new footer in next block
    return pldp;
  }

  // previous and next are both free
  size_t prev_size = GET_SIZE((char*)hdrp - WSIZE);
  void *next_hdrp = (char*)hdrp + size;
  void *prev_hdrp = (char*)hdrp - prev_size;
  size_t next_size = GET_SIZE(next_hdrp);
  WRITE_WORD(prev_hdrp, READ_WORD(prev_hdrp) + size + next_size); // write new header
  WRITE_WORD((char*)next_hdrp + next_size - WSIZE, READ_WORD(prev_hdrp));
  return (char*)prev_hdrp + WSIZE;
}

// linear search
void *find_first_fit(size_t asize) {
#ifdef __HEAP_CHECK__
  writable = 0;
#endif
  void *hdrp = heap_listp;
  size_t b_size;
  do {
    b_size = GET_SIZE(hdrp);
    if (asize <= b_size && !GET_ALLOC(hdrp)) {

#ifdef __HEAP_CHECK__
      writable = 1;
#endif

      return (char*)hdrp + WSIZE;
    }
    hdrp = (char*)hdrp + b_size;
  } while (b_size > 0);

#ifdef __HEAP_CHECK__
      writable = 1;
#endif

  return NULL;
}

void place_and_split(void *pldp, size_t asize) {

  void *hdrp = HDRP_USE_PLDP(pldp);
  size_t b_size = GET_SIZE(hdrp);

#ifdef __HEAP_CHECK__
  assert(b_size >= asize);
#endif

  size_t left_size = b_size - asize;
  // split
  if (left_size && IS_ALIGN_WITH_MIN_BK_SIZE(left_size)) { // reminder > 0 and is multiple of minimum blockasize
    void *new_free_hdrp = (char*)hdrp + asize;

#ifdef __HEAP_CHECK__
    assert(!addr_is_allocated(new_free_hdrp));
#endif
    WRITE_WORD(new_free_hdrp, PACK(left_size, 0));
    WRITE_WORD((char*)new_free_hdrp + left_size - WSIZE, READ_WORD(new_free_hdrp));
    b_size = asize;
  }
  SET_SIZE(hdrp, b_size);
  SET_CURR_ALLOC_BIT(hdrp);
  void *next_hdrp = (char*)hdrp + b_size;
  SET_PREV_ALLOC_BIT(next_hdrp);

  if (!GET_ALLOC(next_hdrp)) {
    SET_PREV_ALLOC_BIT((char*)next_hdrp + GET_SIZE(next_hdrp) - WSIZE);
  }
// #ifdef __HEAP_CHECK__
  // SET_PREV_ALLOC_BIT((char*)next_hdrp + GET_SIZE(next_hdrp) - WSIZE);
// #endif __HEAP_CHECK__
}

void *forward_collect(void *hdrp, size_t *collected_size, size_t target_size) {
  if (GET_PREV_ALLOC(hdrp)) return hdrp;
  size_t old_size = GET_SIZE(hdrp);
  *collected_size = GET_SIZE((char*)hdrp - WSIZE);
  void *ret = (char*)hdrp - *collected_size;
  void *ftrp = (char*)hdrp - WSIZE;
  while (*collected_size + old_size < target_size && !GET_PREV_ALLOC(ret)) {
    size_t next_size = GET_SIZE((char*)ret - WSIZE);
    ret = (char*)ret - next_size;
    WRITE_WORD(ret, READ_WORD(ret) + *collected_size);
    *collected_size = GET_SIZE(ret);
    WRITE_WORD(ftrp, READ_WORD(ret));
  }
  return ret;
}

void *backward_collect(void *hdrp, size_t target_size) {
  void *next_hdrp = (char*)hdrp + GET_SIZE(hdrp);
  while (GET_SIZE(hdrp) < target_size && !GET_ALLOC(next_hdrp)) {
    size_t next_size = GET_SIZE(next_hdrp);
    WRITE_WORD(hdrp, READ_WORD(hdrp) + next_size); // update header
    next_hdrp = (char*)next_hdrp + next_size;
    // WRITE_WORD((char*)next_hdrp - WSIZE, READ_WORD(hdrp));
    SET_PREV_ALLOC_BIT(next_hdrp);
    // CLR_PREV_ALLOC_BIT(next_hdrp);
  }
}

void mm_memcpy(void *dst, void *src, size_t num) {
  char* ddst = (char*)dst;
  char* ssrc = (char*)src;
  size_t i = 0;
  for (i = 0; i < num; ++i) {
    *ddst++ = *ssrc++;
  }
}

void init_free_block(void *hdrp, size_t bk_size) {
  WRITE_WORD(hdrp, PACK(bk_size,0));
  WRITE_WORD((char*)hdrp + bk_size - WSIZE, READ_WORD(hdrp));
  SET_PREV_PTR(hdrp, NULL);
  SET_NEXT_PTR(hdrp, NULL);
}

void insert_into_size_class(void *hdrp, int index) {
#ifdef __LIFO_ORDERING__
  SET_NEXT_PTR(hdrp, segregated_free_list[index]); // hdrp->next = head
  SET_PREV_PTR(hdrp, NULL); // hdrp->prev = NULL
  if (segregated_free_list[index]) { // if (head)
    SET_PREV_PTR(segregated_free_list[index], hdrp); // head->prev = hdrp
  }
  segregated_free_list[index] = hdrp; // head = hdrp
#else // address ordering
  void *head = segregated_free_list[index];
  if (!head) {
    segregated_free_list[index] = hdrp;
  } else if (hdrp < head) {
    SET_NEXT_PTR(hdrp, head); // hdrp->next = head
    SET_PREV_PTR(hdrp, NULL); // hdrp->prev = NULL
    SET_PREV_PTR(head, hdrp); // head->prev = hdrp
    segregated_free_list[index] = hdrp; // head = hdrp
  } else {
    // while (head->next && head->next < hdrp), loop invariant: head < hdrp
    while (GET_NEXT_PTR(head) && GET_NEXT_PTR(head) < hdrp) {
      head = GET_NEXT_PTR(head);
    }
    SET_NEXT_PTR(hdrp, GET_NEXT_PTR(head)); // hdrp->next = head->next
    SET_NEXT_PTR(head, hdrp);               // head->next = hdrp
    SET_PREV_PTR(hdrp, head);               // hdrp->prev = head
    if (GET_NEXT_PTR(hdrp)) {               // if (hdrp->next)
      SET_PREV_PTR(GET_NEXT_PTR(hdrp), hdrp); // hdrp->next->prev = hdrp
    }
  }
#endif
}

void print_list(void *hdrp) {
  fprintf(stderr, "\n\n");
  while (hdrp) {
    fprintf(stderr, "-------------------\n");
    fprintf(stderr, "Block Header: %x, Size = %u, ALLOC Bit = %d\n", hdrp, GET_SIZE(hdrp), GET_ALLOC(hdrp));
    fprintf(stderr, "Previous Ptr: %x, Next Ptr: %x\n", GET_PREV_PTR(hdrp), GET_NEXT_PTR(hdrp));
    hdrp = GET_NEXT_PTR(hdrp);
  }
}

#ifdef __HEAP_CHECK__
void to_binary_str(size_t num, int sep) {
  int size = sizeof(num) * 8;
  int i;
  for (i = size - 1; i >= 0; --i) {
    fprintf(stderr, "%d", !!(num & (1 << i)));
    if (sep && !(i % 4)) {
      fprintf(stderr, " ");
    }
  }
  fprintf(stderr, "\n");
}

void to_hex_str(size_t num, int sep) {
  static size_t mask = (1 << 4) - 1;
  int size = sizeof(num) * 8;
  int i;
  for (i = size - 4; i >= 0; i -= 4) {
    fprintf(stderr, "%x", (num & (mask << i)) >> i);
    if (sep && !(i % 8)) {
      fprintf(stderr, " ");
    }
  }
  fprintf(stderr, "\n");
}

void add_to_alloc_list(const void *ptr, const size_t pl_size, const size_t bk_size) {
  if (ptr == NULL) return;
  assert(!addr_is_allocated(ptr));
  assert(search_list(alloc_list, ptr) == NULL);
  assert(pl_size < bk_size);

  HeapStruct* new_node = (HeapStruct*)malloc(sizeof(HeapStruct));
  new_node->bk_head = HDRP_USE_PLDP(ptr);
  new_node->bk_tail = (char*)HDRP_USE_PLDP(ptr) + bk_size;
  new_node->pl_head = ptr;
  new_node->pl_tail = (char*)ptr + pl_size;
  new_node->bk_size = bk_size;
  new_node->pl_size = pl_size;
  new_node->next = alloc_list; // insert it in the front of the list
  new_node->index = -1;
  alloc_list = new_node;

  assert(new_node->pl_tail - new_node->pl_head == pl_size);
  assert(new_node->bk_tail - new_node->bk_head == bk_size);
  assert(new_node->bk_head < new_node->pl_head);
  assert(new_node->bk_tail >= new_node->pl_tail);
  assert(new_node->pl_tail < (char*)heap_tail);
}

void delete_from_alloc_list(const void *ptr) {
  assert(ptr);
  HeapStruct *node = delete_from_list(alloc_list, ptr);
  assert(node);
  free(node);
}

HeapStruct* delete_from_list(HeapStruct *list, const void *ptr) {
  assert(list);
  HeapStruct *ret = NULL;
  if (list->pl_head == ptr) {
    ret = list;
    alloc_list = list->next;
    ret->next = NULL;
    return ret;
  }

  while (list->next && list->next->pl_head != ptr) {
    list = list->next;
  }

  if (list->next) {
    ret = list->next;
    list->next = ret->next;
    ret->next = NULL;
  }

  return ret;
}

HeapStruct* search_list(const HeapStruct *list, const void *ptr) {
  assert(ptr);
  while (list && list->pl_head != ptr) {
    list = list->next;
  }
  return list;
}

int addr_is_allocated(const char *addr) {
  HeapStruct *alist = alloc_list;
  while (alist) {
    if (addr >= alist->bk_head && addr < alist->bk_tail) {
      return 1;
    }
    alist = alist->next;
  }
  return 0;
}

int addr_is_payload(const char *addr) {
  HeapStruct *alist = alloc_list;
  while (alist) {
    if ((addr >= alist->pl_head) && (addr < alist->pl_tail)) {
      return 1;
    }
    alist = alist->next;
  }
  return 0;
}

int within_heap(const void *addr) {
  return (addr >= heap_head && addr < heap_tail);
}

void show_heap(void) {
  // TODO: change the code, since i change the heap_tail to one byte after
  void* p = heap_head;
  DebugStr("-----------------\n");
  DebugStr("heap_head = %x, heap size = %u\n", p, (char*)heap_tail  - (char*)heap_head);
  DebugStr("-----------------\n");
  DebugStr("%x\n", (char*)p + WSIZE);
  DebugStr("%x\n", (char*)p + 2 * WSIZE);
  DebugStr("-----------------\n");
  p = (char*)p + 3 * WSIZE;
  assert(p < heap_tail);
  while (GET_SIZE(p) > 0) {
    assert(p < heap_tail);
    DebugStr("hdrp:%x val = ", p);
    to_hex_str(*(size_t*)p, 1);
    DebugStr("size = %u, ALLOC = %d, PREV_ALLOC = %d\n", GET_SIZE(p), !!GET_ALLOC(p), !!GET_PREV_ALLOC(p));
    void *ftrp = (char*)p + GET_SIZE(p) - WSIZE;
    DebugStr("ftrp:%x val = ", ftrp);
    to_hex_str(*(size_t*)ftrp, 1);
    if (!GET_ALLOC(p)) { // if current block is not allocated, header = footer
      assert(READ_WORD(p) == READ_WORD(ftrp));
    }
    DebugStr("-----------------\n");
    p += GET_SIZE(p);
  }
  DebugStr("heap_tail = %x\n", p);
  DebugStr("-----------------\n");
}

handler_t *Signal(int signum, handler_t *handler) {
  struct sigaction action, old_action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;

  if (sigaction(signum, &action, &old_action) < 0) {
    fprintf(stderr, "%s\n", strerror(errno));
    exit(1);
  }
  return old_action.sa_handler;
}

void show_alloc_list(void) {
  HeapStruct *alist = alloc_list;
  DebugStr("\n-------------------\n");
  while (alist) {
    DebugStr("Head = %x, Tail = %x, Payload Head = %x, Payload Tail = %x\n",
      alist->bk_head, alist->bk_tail, alist->pl_head, alist->pl_tail);
    DebugStr("-------------------\n");
    alist = alist->next;
  }
}

void print_stack_trace(int signum) {
  void *array[NUM_STACK_TRACE];
  size_t size;
  char **strings;
  size_t i;
  size = backtrace(array, NUM_STACK_TRACE);
  strings = backtrace_symbols(array, size);
  for (i = 1; i < size; ++i) {
    fprintf(stderr, "%s\n", strings[i]);
  }
  free(strings);
}

#endif