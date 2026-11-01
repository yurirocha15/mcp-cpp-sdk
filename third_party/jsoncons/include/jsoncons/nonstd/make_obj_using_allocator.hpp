#ifndef MCP_DETAIL_JSONCONS_NONSTD_MAKE_OBJ_USING_ALLOCATOR
#define MCP_DETAIL_JSONCONS_NONSTD_MAKE_OBJ_USING_ALLOCATOR 

#include <new>			// for placement operator new
#include <tuple>		// for tuple, make_tuple, make_from_tuple
#include <utility>
#include <jsoncons/nonstd/more_type_traits.hpp>

namespace mcp_detail_jsoncons {
namespace nonstd {

template <typename T, typename Alloc, typename... Args>
typename std::enable_if<!nonstd::is_std_pair<T>::value && std::uses_allocator<T, Alloc>::value
    && std::is_constructible<T, Args...,Alloc>::value, T>::type
make_obj_using_allocator(const Alloc& alloc, Args&&... args)
{
    return T(std::forward<Args>(args)..., alloc);
}

template <typename T, typename Alloc, typename... Args>
typename std::enable_if<!nonstd::is_std_pair<T>::value && std::uses_allocator<T, Alloc>::value
    && std::is_constructible<T,std::allocator_arg_t,Alloc,Args...>::value, T>::type
make_obj_using_allocator(const Alloc& alloc, Args&&... args)
{
    return T(std::allocator_arg, alloc, std::forward<Args>(args)...);
}

template <typename T, typename Alloc, typename... Args>
typename std::enable_if<!std::uses_allocator<T, Alloc>::value, T>::type
make_obj_using_allocator(const Alloc&, Args&&... args)
{
    return T(std::forward<Args>(args)...);
}

// std::pair

template <typename T, typename Alloc>
typename std::enable_if<nonstd::is_std_pair<T>::value, T>::type
make_obj_using_allocator(const Alloc& alloc)
{
    return T(
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::first_type>(alloc), 
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::second_type>(alloc));
}

template <typename T, typename Alloc, typename U, typename V>
typename std::enable_if<nonstd::is_std_pair<T>::value, T>::type
make_obj_using_allocator(const Alloc& alloc, U&& u, V&& v)
{
    return T(
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::first_type>(alloc,std::forward<U>(u)), 
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::second_type>(alloc,std::forward<V>(v)));
}

template <typename T, typename Alloc, typename U, typename V>
typename std::enable_if<nonstd::is_std_pair<T>::value, T>::type
make_obj_using_allocator(const Alloc& alloc, const std::pair<U,V>& pr)
{
    return T(
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::first_type>(alloc,pr.first), 
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::second_type>(alloc,pr.second));
}

template <typename T, typename Alloc, typename U, typename V>
typename std::enable_if<nonstd::is_std_pair<T>::value, T>::type
make_obj_using_allocator(const Alloc& alloc, std::pair<U,V>&& pr)
{
    return T(
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::first_type>(alloc,std::move(pr.first)), 
        mcp_detail_jsoncons::nonstd::make_obj_using_allocator<typename T::second_type>(alloc,std::move(pr.second)));
}

} // namespace nonstd
} // namespace mcp_detail_jsoncons

#endif // MCP_DETAIL_JSONCONS_NONSTD_MAKE_OBJ_USING_ALLOCATOR
