#ifndef MYSTL_ALLOCATOR
#define MYSTL_ALLOCATOR

#define __STL_USE_EXCEPTIONS 1
#ifndef THROW_BAD_ALLOC
#  if defined(__STL_NO_BAD_ALLOC) || !defined(__STL_USE_EXCEPTIONS)
#    include <stdio.h>
#    include <stdlib.h>
#    define THROW_BAD_ALLOC fprintf(stderr, "out of memory\n"); exit(1)
#  else /* 标准new的异常 */
#    include <new>
#    define THROW_BAD_ALLOC throw std::bad_alloc()
#  endif
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
namespace lptstl
{
template<int inst>
class malloc_alloc_template
{
private:
    // 以下都是函数指针
    static void *oom_malloc(size_t); // 处理malloc内存不足的情况 
    static void *oom_realloc(void*,size_t); // 处理realloc内存不足的情况
    static void (*malloc_alloc_oom_handler)(); // out-of-memory的处理函数
public:
    static void* allocate(size_t n) 
    {
        void *result = malloc(n); // 第一级配置器直接malloc
        if(nullptr == result) {
            result = oom_malloc(n); // 内存分配不成功时，使用oom_malloc
        }
        return result;
    }
    static void deallocate(void *p, size_t n ) // n未被使用
    {
        free(p);
    }
    static void* reallocate(void *p, size_t old_size,size_t new_size) // old_size 未被使用
    {
        void *result = realloc(p,new_size);
        if(nullptr == result) {
            result = oom_realloc(p,new_size);
        }
        return result;
    }
    static void (* set_malloc_handler(void(*f)()))()
    {
        void (*old)() = malloc_alloc_oom_handler;
        malloc_alloc_oom_handler = f;
        return (old);
    }
};
template<int inst>
void (*malloc_alloc_template<inst>::malloc_alloc_oom_handler)() = 0; // 初值为0

template<int inst>
void *
malloc_alloc_template<inst>::oom_malloc(size_t n)
{
    void(*my_malloc_handler)();
    void *result;
    for(;;) { // try 释放 配置 再释放 再配置
        my_malloc_handler = malloc_alloc_oom_handler;
        if(nullptr == my_malloc_handler) {
            THROW_BAD_ALLOC;
        }
        (*my_malloc_handler)(); // 释放内存
        result = malloc(n);
        if(result) {
            return (result);
        }
    }
}

template<int inst>
void *malloc_alloc_template<inst>::oom_realloc(void *p, size_t n)
{
    void(*my_malloc_handler)();
    void *result;
      for(;;) { // try 释放 配置 再释放 再配置
        my_malloc_handler = malloc_alloc_oom_handler;
        if(nullptr == my_malloc_handler) {
            THROW_BAD_ALLOC;
        }
        (*my_malloc_handler)(); // 释放内存
        result = realloc(p,n);
        if(result) {
            return (result);
        }
    }
}

typedef malloc_alloc_template<0> malloc_alloc;


template<class _Tp, class _Alloc>
class simple_alloc {

public:
    static _Tp* allocate(size_t __n)
      { return 0 == __n ? 0 : (_Tp*) _Alloc::allocate(__n * sizeof (_Tp)); }
    static _Tp* allocate(void)
      { return (_Tp*) _Alloc::allocate(sizeof (_Tp)); }
    static void deallocate(_Tp* __p, size_t __n)
      { if (0 != __n) _Alloc::deallocate(__p, __n * sizeof (_Tp)); }
    static void deallocate(_Tp* __p)
      { _Alloc::deallocate(__p, sizeof (_Tp)); }
};

enum {ALIGN = 8}; // 上调边界
enum {MAX_BYTES = 128}; // 上限
enum {NFREELISTS = MAX_BYTES/ALIGN}; // free-lists 个数
template<bool threads, int inst>
class default_alloc_template {
private:
    static size_t ROUND_UP(size_t bytes) { // 上调bytes至8的倍数
        return (((bytes)+ALIGN-1) & ~(ALIGN-1));
    }
private:
    union obj {
        union obj* free_list_link;
        char client_data[1]; // 客户看见的是第二个
    };
private:
    static obj* volatile free_list[NFREELISTS];

    // 根据区块的大小 决定第n号free-list n从1开始
    static size_t FREELIST_INDEX(size_t bytes) {
        return (((bytes) + ALIGN - 1)/ALIGN - 1);
    }

    // 返回一个大小为n的对象 并可能加入大小为n的其他区块到free list
    static void *refill(size_t n);

    // 配置一大块空间，可容纳nobjs个大小为“size”的区块
    // 如果配置nobjs个区块有所不便 nobjs可能会降低
    static char *chunk_alloc(size_t size, int &nobjs);

    // chunk allocation state
    static char *start_free; // 内存池起始位置 只在chunk_alloc()中变化
    static char *end_free; // 内存池结束位置
    static size_t heap_size;
public:
    static void* allocate(size_t n) {
        obj* volatile *my_free_list;
        obj* result;
        if( n > (size_t) MAX_BYTES) {
            return(malloc_alloc::allocate(n));
        }
        // 寻找适当的free_list
        my_free_list = free_list + FREELIST_INDEX(n);
        result = *my_free_list;
        if(result == nullptr) {
            void *r = refill(ROUND_UP(n));
            return r;
        }
        // 调整free_list
        *my_free_list = result->free_list_link;
        return (result);
    }
    static void* deallocate(void *p, size_t n) {
        obj* q = (obj*)p;
        obj * volatile *my_free_list;
        if( n > (size_t) MAX_BYTES) {
            malloc_alloc::deallocate(p,n);
            return;
        }
        my_free_list = free_list + FREELIST_INDEX(n);
        q->free_list_link = *my_free_list;
        *my_free_list = q;
    }
    static void* reallocate(void *p, size_t old_size, size_t new_size);
};

// 返回大小为n的对象 适当时候为free list增加节点
template<bool threads, int inst>
void*
default_alloc_template<threads,inst>::refill(size_t n)
{
    int nobjs = 20;
    char * chunk = chunk_alloc(n,nobjs);
    obj * volatile *my_free_list;
    obj * result;
    obj * cur_obj, *next_obj;
    int i;
    if(1 == nobjs) {
        return (chunk);
    }
    my_free_list = free_list + FREELIST_INDEX(n);

    result = (obj*) chunk; // 返回给调用者

    *my_free_list = next_obj = (obj*)(chunk+n);
    for(i = 1;;i++) {
        cur_obj = next_obj;
        next_obj = (obj *)((char*)next_obj+n);
        if(nobjs - 1 == i) {
            cur_obj->free_list_link = 0;
            break;
        } else {
            cur_obj->free_list_link = next_obj;
        }
    }
    return result;
}

template<bool threads, int inst>
char *
default_alloc_template<threads,inst>::chunk_alloc(size_t size, int& nobjs)
{
    char * result;
    size_t total_bytes = size * nobjs;
    size_t bytes_left = end_free - start_free;
    if(bytes_left >= total_bytes) { // 此时 内存池的剩余空间完全满足需求量
        result = start_free;
        start_free += total_bytes;
        return (result);
    } else if( bytes_left >= size) { // 不能满足需求量 但是可以提供一个以上的区块
        nobjs = bytes_left/ size;
        total_bytes = size * nobjs;
        result = start_free;
        start_free += total_bytes;
        return (result);
    } else { // 剩余空间不够一个区块
        size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size >> 4);
        if(bytes_left > 0) {
            // 将剩余的空间完全分配给适当的free list
            obj* volatile *my_free_list = free_list + FREELIST_INDEX(bytes_left);
            // 将剩余空间放入 free list
            ((obj*)start_free)->free_list_link = *my_free_list;
            *my_free_list = (obj*)start_free;
        }
    // 从 heap空间里 补充内存池
        start_free = (char*) malloc(bytes_to_get);
        if(0 == start_free) {
            int i;
            obj* volatile *my_free_list ,*p; // 查看已拥有的内存 找寻合适的区块 合适的意思是指 “存在没有使用的区块 并且足够大”
            for(int i = size; i <= MAX_BYTES; i += ALIGN) {
                my_free_list = free_list +  FREELIST_INDEX(bytes_left);
                p = *my_free_list;
                if(0 != p) {
                    *my_free_list = p->free_list_link;
                    start_free = (char *)p;
                    end_free = start_free + i;
                    return (chunk_alloc(size,nobjs)); // 递归调用 修正nobjs 
                    }
            }
            end_free = 0; 
            start_free = (char*)malloc_alloc::allocate(bytes_to_get); // 实在无可用的调用第一级配置器
        } 
        heap_size += bytes_to_get;
        end_free = start_free + bytes_to_get;
        return (chunk_alloc(size,nobjs));
    } 
}
template<bool threads, int inst>
void *
default_alloc_template<threads,inst>::reallocate(void *p, size_t old_size, size_t new_size)
{
    void* result;
    size_t copy_sz;

    if (old_size > (size_t) MAX_BYTES && new_size > (size_t) MAX_BYTES) { // 大于第一级就调用第一级的reallocate
        return(realloc(p, new_size));
    }
    if (ROUND_UP(old_size) == ROUND_UP(new_size)) {
        return(p);
    }
    result = allocate(new_size);
    copy_sz = new_size > old_size? old_size : new_size;
    memcpy(result, p, copy_sz);
    deallocate(p, old_size);
    return(result);
}
template<bool threads, int inst>
char *default_alloc_template<threads, inst>::start_free = 0;

template<bool threads, int inst>
char *default_alloc_template<threads, inst>::end_free = 0;

template<bool threads, int inst>
size_t default_alloc_template<threads, inst>::heap_size = 0;

template<bool threads, int inst>
typename default_alloc_template<threads,inst>::obj *volatile
default_alloc_template<threads,inst>::free_list[NFREELISTS] = 
{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,};
}
#endif





