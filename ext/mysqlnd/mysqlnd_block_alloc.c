/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrey Hristov <andrey@php.net>                             |
  |          Ulf Wendel <uw@php.net>                                     |
  |          Dmitry Stogov <dmitry@zend.com>                             |
  +----------------------------------------------------------------------+
*/

#include "php.h"
#include "mysqlnd.h"
#include "mysqlnd_block_alloc.h"
#include "mysqlnd_debug.h"
#include "mysqlnd_priv.h"


/* {{{ mysqlnd_arena_create */
static zend_always_inline zend_arena* mysqlnd_arena_create(size_t size)
{
	zend_arena *arena = (zend_arena*)mnd_emalloc(size);

	arena->ptr = (char*) arena + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena));
	arena->end = (char*) arena + size;
	arena->prev = NULL;
	return arena;
}
/* }}} */

/* {{{ mysqlnd_arena_destroy */
static zend_always_inline void mysqlnd_arena_destroy(zend_arena *arena)
{
	do {
		zend_arena *prev = arena->prev;
		mnd_efree(arena);
		arena = prev;
	} while (arena);
}
/* }}} */

/* {{{ mysqlnd_arena_alloc */
static zend_always_inline void* mysqlnd_arena_alloc(zend_arena **arena_ptr, size_t size)
{
	zend_arena *arena = *arena_ptr;
	char *ptr = arena->ptr;

	size = ZEND_MM_ALIGNED_SIZE(size);

	if (EXPECTED(size <= (size_t)(arena->end - ptr))) {
		arena->ptr = ptr + size;
	} else {
		size_t arena_size =
			UNEXPECTED((size + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena))) > (size_t)(arena->end - (char*) arena)) ?
				(size + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena))) :
				(size_t)(arena->end - (char*) arena);
		zend_arena *new_arena = (zend_arena*)mnd_emalloc(arena_size);

		ptr = (char*) new_arena + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena));
		new_arena->ptr = (char*) new_arena + ZEND_MM_ALIGNED_SIZE(sizeof(zend_arena)) + size;
		new_arena->end = (char*) new_arena + arena_size;
		new_arena->prev = arena;
		*arena_ptr = new_arena;
	}

	return (void*) ptr;
}
/* }}} */

/* {{{ mysqlnd_mempool_free_chunk */
static void
mysqlnd_mempool_free_chunk(MYSQLND_MEMORY_POOL * pool, MYSQLND_MEMORY_POOL_CHUNK * chunk)
{
	DBG_ENTER("mysqlnd_mempool_free_chunk");
	/* Try to back-off and guess if this is the last block allocated */
	if ((char*)chunk == (char*)pool->arena->ptr - ZEND_MM_ALIGNED_SIZE(sizeof(MYSQLND_MEMORY_POOL_CHUNK) + chunk->size)) {
		/*
			This was the last allocation. Lucky us, we can free
			a bit of memory from the pool. Next time we will return from the same ptr.
		*/
		pool->arena->ptr = (char*)chunk;
	}
	DBG_VOID_RETURN;
}
/* }}} */


/* {{{ mysqlnd_mempool_resize_chunk */
static MYSQLND_MEMORY_POOL_CHUNK *
mysqlnd_mempool_resize_chunk(MYSQLND_MEMORY_POOL * pool, MYSQLND_MEMORY_POOL_CHUNK * chunk, unsigned int size)
{
	DBG_ENTER("mysqlnd_mempool_resize_chunk");

	/* Try to back-off and guess if this is the last block allocated */
	if (((char*)chunk == (char*)pool->arena->ptr - ZEND_MM_ALIGNED_SIZE(sizeof(MYSQLND_MEMORY_POOL_CHUNK) + chunk->size))
	  && (ZEND_MM_ALIGNED_SIZE(sizeof(MYSQLND_MEMORY_POOL_CHUNK) + size) <= ((char*)pool->arena->end - (char*)chunk))) {
		/*
			This was the last allocation. Lucky us, we can free
			a bit of memory from the pool. Next time we will return from the same ptr.
		*/
		pool->arena->ptr = (char*)chunk + ZEND_MM_ALIGNED_SIZE(sizeof(MYSQLND_MEMORY_POOL_CHUNK) + size);
	} else {
		MYSQLND_MEMORY_POOL_CHUNK *new_chunk = mysqlnd_arena_alloc(&pool->arena, sizeof(MYSQLND_MEMORY_POOL_CHUNK) + size);
		memcpy(new_chunk, chunk, sizeof(MYSQLND_MEMORY_POOL_CHUNK) + MIN(size, chunk->size));
		chunk = new_chunk;
	}
	chunk->size = size;
	DBG_RETURN(chunk);
}
/* }}} */


/* {{{ mysqlnd_mempool_get_chunk */
static MYSQLND_MEMORY_POOL_CHUNK *
mysqlnd_mempool_get_chunk(MYSQLND_MEMORY_POOL * pool, unsigned int size)
{
	MYSQLND_MEMORY_POOL_CHUNK *chunk = NULL;
	DBG_ENTER("mysqlnd_mempool_get_chunk");

	chunk = mysqlnd_arena_alloc(&pool->arena, sizeof(MYSQLND_MEMORY_POOL_CHUNK) + size);
	chunk->size = size;

	DBG_RETURN(chunk);
}
/* }}} */


/* {{{ mysqlnd_mempool_create */
PHPAPI MYSQLND_MEMORY_POOL *
mysqlnd_mempool_create(size_t arena_size)
{
	zend_arena * arena;
	MYSQLND_MEMORY_POOL * ret;

	DBG_ENTER("mysqlnd_mempool_create");
	arena = mysqlnd_arena_create(MAX(arena_size, sizeof(zend_arena)));
	ret = mysqlnd_arena_alloc(&arena, sizeof(MYSQLND_MEMORY_POOL));
	ret->arena = arena;
	ret->get_chunk = mysqlnd_mempool_get_chunk;
	ret->free_chunk = mysqlnd_mempool_free_chunk;
	ret->resize_chunk = mysqlnd_mempool_resize_chunk;
	DBG_RETURN(ret);
}
/* }}} */


/* {{{ mysqlnd_mempool_destroy */
PHPAPI void
mysqlnd_mempool_destroy(MYSQLND_MEMORY_POOL * pool)
{
	DBG_ENTER("mysqlnd_mempool_destroy");
	/* mnd_free will reference LOCK_access and might crash, depending on the caller...*/
	mysqlnd_arena_destroy(pool->arena);
	DBG_VOID_RETURN;
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
