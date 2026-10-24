#ifndef KERNEL_TYPES_HPP
#define KERNEL_TYPES_HPP

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

typedef __INT8_TYPE__ int8_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT64_TYPE__ uint64_t;

typedef __INTPTR_TYPE__ intptr_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef __INT64_TYPE__ ssize_t;

typedef decltype(nullptr) nullptr_t;

// Placement new operator
inline void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

inline void* operator new[](size_t, void* ptr) noexcept {
    return ptr;
}

#endif // KERNEL_TYPES_HPP
