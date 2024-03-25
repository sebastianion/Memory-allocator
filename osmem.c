// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

#define MMAP_THRESHOLD (128 * 1024)
#define ALIGN8(size) (((size) + 7) & ~7)
#define SIZEOF_STRUCT_BLOCK_META ALIGN8(sizeof(struct block_meta))


void *mem_begin;
struct block_meta *mem_end;


/* split the given block */
void split_block(struct block_meta *block, size_t size)
{
	// align the new size
	size_t new_size_aligned = ALIGN8(size + SIZEOF_STRUCT_BLOCK_META);

	// access the position where the newly created block should be;
	// a cast to (char *) is needed and after that we can just add
	// the new size as an offset;
	struct block_meta *temp = block->next;
	struct block_meta *second_part = (struct block_meta *)
									((char *) block + new_size_aligned);

	// add the struct to the list and fill the info needed
	second_part->next = temp;
	second_part->size = ALIGN8(block->size - new_size_aligned);
	second_part->status = STATUS_FREE;
	block->next = second_part;
	block->size = ALIGN8(size);

	// update the end of the list if the last block was in need of a split
	if (mem_end == block)
		mem_end = second_part;
}

/* merge multiple blocks */
void coalesce_blocks(void)
{
	// get the list head
	struct block_meta *curr = mem_begin;
	struct block_meta *temp = NULL;

	// while both the current and the next block are free, merge them;
	// if a merge has been made, attempt to only change the next block and
	// check if it is free as well; if that is the case, continue the merge
	// process;
	while (curr && curr->next) {
		if (curr->status == STATUS_FREE && curr->next->status == STATUS_FREE) {
			// use a temporary variable to store the block that is going to
			// not appear in the list anymore
			temp = curr->next;
			curr->next = curr->next->next;
			// update the size of the current by adding the size
			// of the former node and the size of the block_meta struct,
			// as that counts as available space as well
			curr->size += temp->size + SIZEOF_STRUCT_BLOCK_META;
			curr->size = ALIGN8(curr->size);
		} else
			// if one of the blocks checked is not free, then update the
			// current node and repeat the process
			curr = curr->next;
	}

	// try to update the last block, if anything changed over there
	curr = mem_begin;
	while (curr && curr->next)
		curr = curr->next;

	mem_end = curr;
}

/* algorithm to find the best fitting block in the list */
struct block_meta *find_best_block(size_t size)
{
	// begin search from the head
	if (!mem_begin)
		return NULL;

	// merge the free blocks
	coalesce_blocks();

	struct block_meta *curr = mem_begin;

	// if there is at least a block in the list, initialize the result
	// with the first block in the list (and check if it is free to use!)
	size_t best_fit_size = curr->size;
	struct block_meta *best = curr;
	size_t can_be_used = best->status == STATUS_FREE ? 1 : 0;

	// we search for the block that fits our size best;
	// for that, we iterate through the whole list
	while (curr) {
		if (curr->status == STATUS_FREE && curr->size >= size &&
			// this last condition is needed because:
			// 1. if we find a block with a better fit than the best found size
			// until now, we can update the best block
			// 2. if we find a block with a worse fit than the best found size
			// until now, we need to see if the block that had that best size
			// can be used; if not, we can update the best block
			(curr->size < best_fit_size || can_be_used == 0)) {
			best = curr;
			best_fit_size = curr->size;
			// this block can for sure be used
			can_be_used = 1;
		}

		curr = curr->next;
	}

	size_t size_aligned = ALIGN8(size + SIZEOF_STRUCT_BLOCK_META);

	// if we have found a block we can use, check if it needs to be split
	if (can_be_used == 1 && best->size > size_aligned)
		split_block(best, size);

	// if we have found a block we can use, return it
	return can_be_used == 1 ? best : NULL;
}


/* create a new memory block */
struct block_meta *create_block(size_t size, size_t treshold)
{
	struct block_meta *new_block = NULL;

	// if the given size is smaller than the treshold value, use sbrk;
	// otherwise, use mmap
	if (ALIGN8(size) < treshold) {
		// source: man sbrk;
		// increase heap size by calling sbrk(size + SIZEOF_STRUCT_BLOCK_META);

		new_block = sbrk(ALIGN8(size + SIZEOF_STRUCT_BLOCK_META));

		// check the error code
		DIE(new_block == (void *) -1, "sbrk failed");
		new_block->status = STATUS_ALLOC;

	} else {
		// allocate independent memory chunk
		new_block = mmap(NULL, ALIGN8(size + SIZEOF_STRUCT_BLOCK_META),
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		// check the error code
		DIE(new_block == MAP_FAILED, "map failed");
		new_block->status = STATUS_MAPPED;
	}

	// align the new block's size and set the next value to NULL
	new_block->size = ALIGN8(size);
	new_block->next = NULL;

	return new_block;
}


/* alloc a number of bytes on the heap when first using it */
struct block_meta *prealloc(void)
{
	// create a new block of MMAP_THRESHOLD size
	struct block_meta *new_block = create_block(MMAP_THRESHOLD -
									SIZEOF_STRUCT_BLOCK_META, MMAP_THRESHOLD);

	if (!new_block)
		return NULL;

	// initialize the list
	mem_begin = new_block;
	mem_end = mem_begin;

	// return the payload
	return (new_block + 1);
}


/* auxiliary malloc function that takes
 * a treshold value as an extra parameter
 */
void *os_malloc_aux(size_t size, size_t treshold)
{
	// early exit case
	if (size <= 0)
		return NULL;

	// align the size
	size_t size_aligned = ALIGN8(size);
	// get a new block that will hold the memory
	struct block_meta *new_block = NULL;

	// check the existing memory list for a fitting value if the size it needs
	// is small enough to be allocated with sbrk
	if (mem_begin && size_aligned < treshold) {
		new_block = find_best_block(size_aligned);

		if (new_block) {
			// if we have found a block, mark it as allocated and return
			// the payload, that is adding 1 to the block_meta struct
			new_block->status = STATUS_ALLOC;
			return (new_block + 1);
		}
	}

	// if we have not found a fitting block in the list, we can check
	// the last block to see if it is free and, if so, expand it and use it
	if (mem_end && mem_end->status == STATUS_FREE &&
		mem_end->size < size_aligned &&
		size_aligned < treshold - SIZEOF_STRUCT_BLOCK_META) {
		// get more space by computing the needed extra size
		struct block_meta *res = sbrk(ALIGN8(size_aligned - mem_end->size));

		// check the error code
		DIE(res == (void *) -1, "sbrk failed");

		mem_end->size = ALIGN8(size_aligned);
		mem_end->status = STATUS_ALLOC;
		return mem_end + 1;
	}

	// if the list has not been used yet, prealloc memory on heap
	if (size_aligned < treshold - SIZEOF_STRUCT_BLOCK_META && !mem_begin)
		return prealloc();

	// if none of the above cases were a match, create the block, at last
	new_block = create_block(size_aligned,
							treshold - SIZEOF_STRUCT_BLOCK_META);
	if (!new_block)
		return NULL;

	// we requested a new block so we update the list if sbrk() was used
	if (new_block->status == STATUS_ALLOC) {
		mem_end->next = new_block;
		mem_end = new_block;
	}

	// return the payload
	return (new_block + 1);
}


/* calls os_malloc_aux with MMAP_THRESHOLD as treshold value */
void *os_malloc(size_t size)
{
	/* TODO: Implement os_malloc */
	return os_malloc_aux(size, MMAP_THRESHOLD);
}


/* calls os_malloc_aux with page_size as treshold value
 * and sets every byte to 0
 */
void *os_calloc(size_t nmemb, size_t size)
{
	/* TODO: Implement os_calloc */

	// get the page size
	long page_size = getpagesize();

	void *out = os_malloc_aux(nmemb * size, page_size);

	// set bytes to 0
	memset((void *) out, 0, nmemb * size);
	return out;
}

/* change the size of the memory block to "size" bytes */
void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */

	// early exit cases
	if (ptr == NULL)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	// get the block_meta structure from the given pointer;
	// to do that, we need to cast the pointer and after that
	// substract 1, as ptr is only the payload and we have to
	// extract the metadata as well;
	struct block_meta *block = ((struct block_meta *) ptr) - 1;

	// align the new size and old size;
	// these have the size of the struct added to them as well
	size_t new_size_aligned = ALIGN8(size + SIZEOF_STRUCT_BLOCK_META);
	size_t old_size_aligned = ALIGN8(block->size + SIZEOF_STRUCT_BLOCK_META);

	// if the block we are trying to realloc is not being used, return NULL
	if (block->status == STATUS_FREE)
		return NULL;

	// if the block we are trying to realloc is also the last one and
	// the new size is smaller than MMAP_THRESHOLD, we can use sbrk to allocate
	// it by expanding the last block, similar to what we did before
	if (block == mem_end && old_size_aligned < new_size_aligned &&
		ALIGN8(size) < MMAP_THRESHOLD - SIZEOF_STRUCT_BLOCK_META) {
		struct block_meta *res = sbrk(ALIGN8(size - mem_end->size));

		// check the error code
		DIE(res == (void *) -1, "sbrk failed");

		mem_end->size = ALIGN8(size);
		mem_end->status = STATUS_ALLOC;
		return mem_end + 1;
	}

	// coalesce blocks until we can fit the new size
	if (old_size_aligned < new_size_aligned &&
		new_size_aligned < MMAP_THRESHOLD) {
		struct block_meta *temp = NULL;

		// while the next block exists and is free try to merge the blocks
		while (block->next && block->next->status == STATUS_FREE) {
			temp = block->next;

			// if the size of the new block would become bigger than
			// MMAP_THRESHOLD, quit the merging algorithm, as that
			// new block would become too big to be stored in the list
			if (ALIGN8(block->size + temp->size + SIZEOF_STRUCT_BLOCK_META) >
				MMAP_THRESHOLD)
				break;

			// go to the next block and update the base block
			block->next = block->next->next;
			block->size += temp->size + SIZEOF_STRUCT_BLOCK_META;
			block->size = ALIGN8(block->size);

			// at last, if the block becomes big enough to fit the new size
			// quit the merging algorithm;
			if (ALIGN8(block->size + SIZEOF_STRUCT_BLOCK_META) >=
				new_size_aligned)
				break;
		}
	}

	// update the size of the base block
	old_size_aligned = ALIGN8(block->size + SIZEOF_STRUCT_BLOCK_META);

	// if the sizes are now equal, our job is done, return the original pointer
	if (old_size_aligned == new_size_aligned)
		return ptr;

	// if the block is big enough to be split, do it
	if (old_size_aligned > new_size_aligned + SIZEOF_STRUCT_BLOCK_META) {
		// if is has been allocated with mmap, alloc a new block
		// and free the old one
		if (block->status == STATUS_MAPPED) {
			void *newptr = os_malloc(size);

			if (!newptr)
				return NULL;

			// copy the contens from the old address
			memcpy(newptr, ptr, new_size_aligned);
			os_free(ptr);

			return newptr;
		}

		// if sbrk has been used, simply split the block
		if (block->status == STATUS_ALLOC) {
			split_block(block, size);

			return ptr;
		}
	}

	// if the block is not big enough to be split, just return the pointer
	if (old_size_aligned > new_size_aligned)
		return ptr;

	// if no previous cases have matched our case, simply alloc a new block
	// of memory and copy over the content from the old address
	void *newptr = os_malloc(size);

	if (!newptr)
		return NULL;

	memcpy(newptr, ptr, old_size_aligned);

	if (newptr != ptr)
		os_free(ptr);

	return newptr;
}

/* frees memory allocated by os_malloc(), os_calloc() or os_realloc() */
void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (!ptr)
		return;

	// get the block_meta structure from the given pointer
	struct block_meta *to_free_block = ((struct block_meta *) ptr) - 1;

	// if the block has been allocated with sbrk, just mark is as free
	if (to_free_block->status == STATUS_ALLOC)
		to_free_block->status = STATUS_FREE;

	// otherwise, change the flag and also call munmap
	else if (to_free_block->status == STATUS_MAPPED) {
		to_free_block->status = STATUS_FREE;

		int res = munmap(to_free_block, to_free_block->size +
							SIZEOF_STRUCT_BLOCK_META);

		DIE(res == -1, "munmap fail");
	}
}
