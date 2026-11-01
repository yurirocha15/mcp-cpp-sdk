// Copyright 2013-2026 Daniel Parker
// Distributed under the Boost license, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// See https://github.com/danielaparker/jsoncons for latest version

#ifndef MCP_DETAIL_JSONCONS_REFLECT_REFLECT_TRAITS_GEN_HPP
#define MCP_DETAIL_JSONCONS_REFLECT_REFLECT_TRAITS_GEN_HPP

#include <utility>
#include <bitset>

#include <jsoncons/nonstd/compiler_support.hpp>
#include <jsoncons/config/jsoncons_config.hpp> // MCP_DETAIL_JSONCONS_PP_EXPAND, MCP_DETAIL_JSONCONS_PP_QUOTE
#include <jsoncons/conv_error.hpp>
#include <jsoncons/conversion_result.hpp>
#include <jsoncons/json_exception.hpp>
#include <jsoncons/json_visitor.hpp>
#include <jsoncons/reflect/decode_traits.hpp>
#include <jsoncons/reflect/encode_traits.hpp>
#include <jsoncons/reflect/json_traits.hpp>
#include <jsoncons/semantic_tag.hpp>
#include <jsoncons/ser_common.hpp>
#include <jsoncons/nonstd/more_type_traits.hpp>
#include <jsoncons/utility/function_objects.hpp>

#define MCP_DETAIL_JSONCONS_RDONLY(X)

#define MCP_DETAIL_JSONCONS_RDWR(X) X

namespace mcp_detail_jsoncons {
namespace reflect {

inline
std::string generate_msg_arg(const char* class_name, const char* s)
{
    std::string str;
    str.append(class_name);
    str.push_back(':');
    str.push_back(':');
    str.append(s);
    return str;
}

inline
std::string generate_msg_arg(const char* class_name, const wchar_t* s)
{
    std::string str;
    str.append(class_name);
    str.push_back(':');
    str.push_back(':');
    std::size_t len = std::char_traits<wchar_t>::length(s);
    mcp_detail_jsoncons::unicode_traits::convert(s, len, str);
    return str;
}

template <typename CharT>
basic_string_view<CharT> get_key(basic_staj_cursor<CharT>& cursor, std::error_code& ec)
{ 
    using string_view_type = basic_string_view<CharT>;

    if (cursor.current().event_type() != staj_events::key)
    {
        ec = conv_errc::not_key;
        return string_view_type{};
    }
 
    return cursor.current().template get<string_view_type>(); 
}

template <typename CharT>
bool read_next_or_end(basic_staj_cursor<CharT>& cursor, std::error_code& ec)
{ 
    cursor.next(ec); 
    if (ec) 
    { 
        return true; 
    } 
    return (cursor.current().event_type() == staj_events::end_object) ? true : false; 
}

template <std::size_t N>
std::size_t find_first_not_set(const std::bitset<N>& indices)
{
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (!indices[i]) {
            return i;
        }
    }
    return indices.size();
}

template <typename T,typename CharT>
struct object_names
{};

template <typename T>
struct error_context
{};

template <typename T>
struct reflect_type_properties
{};

template <typename T, typename U> 
void set_member(T&&, const U&) 
{ 
} 
template <typename T, typename U> 
void set_member(T&& val, U& result)
{ 
    result = std::forward<T>(val); 
} 

template <typename Json>
struct json_traits_helper
{
    using string_view_type = typename Json::string_view_type;

    template <typename T,typename Alloc,typename TempAlloc> 
    static conversion_result<T> try_as_value(const allocator_set<Alloc,TempAlloc>& aset, 
        const Json& j) 
    { 
        auto result = j.template try_as<T>(aset); 
        if (!result)
        {
            return conversion_result<T>(unexpect, result.error());
        }
        return conversion_result<T>(std::move(*result));
    } 

    template <typename U> 
    static void set_optional_json_member(string_view_type key, const std::shared_ptr<U>& val, Json& j) 
    { 
        if (val) j.try_emplace(key, val); 
    } 
    template <typename U,typename Deleter> 
    static void set_optional_json_member(string_view_type key, const std::unique_ptr<U,Deleter>& val, Json& j) 
    { 
        if (val) j.try_emplace(key, val); 
    } 
    template <typename U> 
    static 
    typename std::enable_if<nonstd::is_optional<U>::value, void>::type
    set_optional_json_member(string_view_type key, const U& val, Json& j) 
    { 
        if (val.has_value()) j.try_emplace(key, val); 
    } 
    template <typename U> 
    static         
    typename std::enable_if<!nonstd::is_optional<U>::value, void>::type
    set_optional_json_member(string_view_type key, const U& val, Json& j) 
    { 
        j.try_emplace(key, val); 
    } 
};

template <typename CharT, typename T> 
write_result try_encode_member(const basic_string_view<CharT>& key, const T& val, basic_json_visitor<CharT>& encoder) 
{ 
    encoder.key(key);
    return encode_traits<T>::encode(make_alloc_set(), val, encoder); 
} 

template <typename CharT, typename T> 
write_result try_encode_optional_member(const basic_string_view<CharT>& key, const std::shared_ptr<T>& val, basic_json_visitor<CharT>& encoder) 
{ 
    if (val) 
    {
        encoder.key(key);
        return encode_traits<T>::encode(make_alloc_set(), *val, encoder); 
    }
    return write_result{}; 
}
 
template <typename CharT, typename T,typename Deleter> 
write_result try_encode_optional_member(const basic_string_view<CharT>& key, const std::unique_ptr<T,Deleter>& val, basic_json_visitor<CharT>& encoder) 
{ 
    if (val)
    {
        encoder.key(key);
        return encode_traits<T>::encode(make_alloc_set(), *val, encoder); 
    }
    return write_result{}; 
}
 
template <typename CharT, typename T> 
typename std::enable_if<nonstd::is_optional<T>::value, write_result>::type
try_encode_optional_member(const basic_string_view<CharT>& key, const T& val, basic_json_visitor<CharT>& encoder) 
{ 
    if (val.has_value())
    {
        encoder.key(key);
        return encode_traits<T>::encode(make_alloc_set(), *val, encoder); 
    }
    return write_result{}; 
} 

template <typename CharT, typename T> 
typename std::enable_if<!nonstd::is_optional<T>::value, write_result>::type
try_encode_optional_member(const basic_string_view<CharT>& key, const T& val, basic_json_visitor<CharT>& encoder)
{ 
    encoder.key(key);
    return encode_traits<T>::encode(make_alloc_set(), val, encoder); 
} 

template <typename T> 
bool is_optional_value_set(const std::shared_ptr<T>& val) 
{ 
    return val ? true : false; 
} 
template <typename T,typename Deleter> 
bool is_optional_value_set(const std::unique_ptr<T,Deleter>& val) 
{ 
    return val ? true : false;
} 
template <typename T> 
typename std::enable_if<nonstd::is_optional<T>::value, bool>::type
is_optional_value_set(const T& val) 
{ 
    return val.has_value();
} 
template <typename T> 
typename std::enable_if<!nonstd::is_optional<T>::value, bool>::type
is_optional_value_set(const T&) 
{
    return true; 
} 

} // namespace reflect

} // namespace mcp_detail_jsoncons

#if defined(_MSC_VER)
#pragma warning( disable : 4127)
#endif

#define MCP_DETAIL_JSONCONS_PP_CONCAT_IMPL(a, b) a ## b
#define MCP_DETAIL_JSONCONS_PP_CONCAT(a, b) MCP_DETAIL_JSONCONS_PP_CONCAT_IMPL(a, b)

// Inspired by https://github.com/Loki-Astari/ThorsSerializer/blob/master/src/Serialize/Traits.h

#define MCP_DETAIL_JSONCONS_NARGS(...) MCP_DETAIL_JSONCONS_NARG_(__VA_ARGS__, 70,69,68,67,66,65,64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define MCP_DETAIL_JSONCONS_NARG_(...) MCP_DETAIL_JSONCONS_PP_EXPAND( MCP_DETAIL_JSONCONS_ARG_N(__VA_ARGS__) )
#define MCP_DETAIL_JSONCONS_ARG_N(e1,e2,e3,e4,e5,e6,e7,e8,e9,e10,e11,e12,e13,e14,e15,e16,e17,e18,e19,e20,e21,e22,e23,e24,e25,e26,e27,e28,e29,e30,e31,e32,e33,e34,e35,e36,e37,e38,e39,e40,e41,e42,e43,e44,e45,e46,e47,e48,e49,e50,e51,e52,e53,e54,e55,e56,e57,e58,e59,e60,e61,e62,e63,e64,e65,e66,e67,e68,e69,e70,N,...)N

#define MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, Count) Call(P1, P2, P3, P4, Count) 
 
#define MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(Call, P1, P2, P3, ...)         MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_N(Call, P1,P2, P3, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__), __VA_ARGS__)
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_N(Call, P1, P2, P3, Count, ...)  MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_N_(Call, P1, P2, P3, Count, __VA_ARGS__)
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_N_(Call, P1, P2, P3, Count, ...) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_ ## Count(Call, P1, P2, P3, __VA_ARGS__))

#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_70(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 70) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_69(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_69(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 69) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_68(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_68(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 68) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_67(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_67(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 67) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_66(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_66(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 66) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_65(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_65(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 65) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_64(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_64(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 64) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_63(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_63(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 63) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_62(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_62(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 62) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_61(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_61(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 61) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_60(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_60(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 60) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_59(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_59(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 59) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_58(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_58(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 58) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_57(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_57(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 57) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_56(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_56(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 56) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_55(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_55(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 55) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_54(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_54(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 54) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_53(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_53(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 53) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_52(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_52(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 52) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_51(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_51(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 51) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_50(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_50(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 50) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_49(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_49(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 49) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_48(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_48(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 48) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_47(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_47(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 47) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_46(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_46(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 46) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_45(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_45(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 45) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_44(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_44(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 44) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_43(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_43(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 43) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_42(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_42(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 42) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_41(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_41(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 41) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_40(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_40(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 40) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_39(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_39(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 39) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_38(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_38(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 38) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_37(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_37(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 37) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_36(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_36(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 36) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_35(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_35(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 35) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_34(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_34(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 34) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_33(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_33(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 33) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_32(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_32(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 32) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_31(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_31(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 31) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_30(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_30(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 30) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_29(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_29(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 29) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_28(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_28(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 28) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_27(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_27(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 27) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_26(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_26(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 26) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_25(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_25(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 25) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_24(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_24(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 24) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_23(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_23(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 23) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_22(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_22(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 22) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_21(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_21(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 21) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_20(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_20(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 20) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_19(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_19(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 19) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_18(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_18(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 18) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_17(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_17(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 17) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_16(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_16(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 16) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_15(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_15(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 15) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_14(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_14(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 14) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_13(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_13(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 13) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_12(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_12(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 12) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_11(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_11(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 11) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_10(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_10(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 10) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_9(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_9(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 9) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_8(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_8(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 8) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_7(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_7(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 7) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_6(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_6(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 6) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_5(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_5(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 5) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_4(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_4(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 4) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_3(Call, P1, P2, P3, __VA_ARGS__))
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_3(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 3) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_2(Call, P1, P2, P3, __VA_ARGS__)) 
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_2(Call, P1, P2, P3, P4, ...)    MCP_DETAIL_JSONCONS_EXPAND_CALL5(Call, P1, P2, P3, P4, 2) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_1(Call, P1, P2, P3, __VA_ARGS__)) 
#define MCP_DETAIL_JSONCONS_VARIADIC_REP_OF_1(Call, P1, P2, P3, P4)         MCP_DETAIL_JSONCONS_PP_EXPAND(Call ## _LAST(P1, P2, P3, P4, 1))

#define MCP_DETAIL_JSONCONS_TYPE_TRAITS_FRIEND \
    template <typename JSON,typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::json_type_traits; \
    template <typename JSON,typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::json_traits; \
    template <typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::encode_traits; \
    template <typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::decode_traits;

#define MCP_DETAIL_JSONCONS_CONV_TRAITS_FRIEND \
    template <typename JSON,typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::json_traits; \
    template <typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::encode_traits; \
    template <typename T,typename Enable> \
    friend struct mcp_detail_jsoncons::reflect::decode_traits;

#define MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, Id) MCP_DETAIL_JSONCONS_PP_EXPAND(Call(Expr, Id))

#define MCP_DETAIL_JSONCONS_REP_OF_N(Call, Expr, Pre, App, Count)  MCP_DETAIL_JSONCONS_REP_OF_ ## Count(Call, Expr, Pre, App)

#define MCP_DETAIL_JSONCONS_REP_OF_50(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 50) MCP_DETAIL_JSONCONS_REP_OF_49(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_49(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 49) MCP_DETAIL_JSONCONS_REP_OF_48(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_48(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 48) MCP_DETAIL_JSONCONS_REP_OF_47(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_47(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 47) MCP_DETAIL_JSONCONS_REP_OF_46(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_46(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 46) MCP_DETAIL_JSONCONS_REP_OF_45(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_45(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 45) MCP_DETAIL_JSONCONS_REP_OF_44(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_44(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 44) MCP_DETAIL_JSONCONS_REP_OF_43(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_43(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 43) MCP_DETAIL_JSONCONS_REP_OF_42(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_42(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 42) MCP_DETAIL_JSONCONS_REP_OF_41(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_41(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 41) MCP_DETAIL_JSONCONS_REP_OF_40(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_40(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 40) MCP_DETAIL_JSONCONS_REP_OF_39(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_39(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 39) MCP_DETAIL_JSONCONS_REP_OF_38(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_38(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 38) MCP_DETAIL_JSONCONS_REP_OF_37(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_37(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 37) MCP_DETAIL_JSONCONS_REP_OF_36(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_36(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 36) MCP_DETAIL_JSONCONS_REP_OF_35(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_35(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 35) MCP_DETAIL_JSONCONS_REP_OF_34(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_34(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 34) MCP_DETAIL_JSONCONS_REP_OF_33(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_33(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 33) MCP_DETAIL_JSONCONS_REP_OF_32(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_32(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 32) MCP_DETAIL_JSONCONS_REP_OF_31(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_31(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 31) MCP_DETAIL_JSONCONS_REP_OF_30(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_30(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 30) MCP_DETAIL_JSONCONS_REP_OF_29(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_29(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 29) MCP_DETAIL_JSONCONS_REP_OF_28(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_28(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 28) MCP_DETAIL_JSONCONS_REP_OF_27(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_27(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 27) MCP_DETAIL_JSONCONS_REP_OF_26(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_26(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 26) MCP_DETAIL_JSONCONS_REP_OF_25(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_25(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 25) MCP_DETAIL_JSONCONS_REP_OF_24(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_24(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 24) MCP_DETAIL_JSONCONS_REP_OF_23(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_23(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 23) MCP_DETAIL_JSONCONS_REP_OF_22(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_22(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 22) MCP_DETAIL_JSONCONS_REP_OF_21(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_21(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 21) MCP_DETAIL_JSONCONS_REP_OF_20(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_20(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 20) MCP_DETAIL_JSONCONS_REP_OF_19(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_19(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 19) MCP_DETAIL_JSONCONS_REP_OF_18(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_18(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 18) MCP_DETAIL_JSONCONS_REP_OF_17(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_17(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 17) MCP_DETAIL_JSONCONS_REP_OF_16(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_16(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 16) MCP_DETAIL_JSONCONS_REP_OF_15(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_15(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 15) MCP_DETAIL_JSONCONS_REP_OF_14(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_14(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 14) MCP_DETAIL_JSONCONS_REP_OF_13(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_13(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 13) MCP_DETAIL_JSONCONS_REP_OF_12(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_12(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 12) MCP_DETAIL_JSONCONS_REP_OF_11(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_11(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 11) MCP_DETAIL_JSONCONS_REP_OF_10(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_10(Call, Expr, Pre, App)     Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 10) MCP_DETAIL_JSONCONS_REP_OF_9(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_9(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 9) MCP_DETAIL_JSONCONS_REP_OF_8(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_8(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 8) MCP_DETAIL_JSONCONS_REP_OF_7(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_7(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 7) MCP_DETAIL_JSONCONS_REP_OF_6(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_6(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 6) MCP_DETAIL_JSONCONS_REP_OF_5(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_5(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 5) MCP_DETAIL_JSONCONS_REP_OF_4(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_4(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 4) MCP_DETAIL_JSONCONS_REP_OF_3(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_3(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 3) MCP_DETAIL_JSONCONS_REP_OF_2(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_2(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call, Expr, 2) MCP_DETAIL_JSONCONS_REP_OF_1(Call, Expr, , App)
#define MCP_DETAIL_JSONCONS_REP_OF_1(Call, Expr, Pre, App)      Pre MCP_DETAIL_JSONCONS_EXPAND_CALL2(Call ## _LAST, Expr, 1) App
#define MCP_DETAIL_JSONCONS_REP_OF_0(Call, Expr, Pre, App)

#define MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(Call, Count) MCP_DETAIL_JSONCONS_REP_OF_N(Call, , , ,Count)
#define MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(Call, Count) MCP_DETAIL_JSONCONS_REP_OF_N(Call, ,<,>,Count)
#define MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM(Expr, Id) typename T ## Id,
#define MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM_LAST(Expr, Id) typename T ## Id
#define MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM(Expr, Id) ,typename T ## Id
#define MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM_LAST(Expr, Id) ,typename T ## Id
#define MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG(Expr, Id) T ## Id,
#define MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG_LAST(Ex, Id) T ## Id 


#define MCP_DETAIL_JSONCONS_GENERATE_NAME(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_GENERATE_NAME_LAST(Prefix, P2, P3, Member, Count) 
#define MCP_DETAIL_JSONCONS_GENERATE_NAME_LAST(Prefix, P2, P3, Member, Count) \
    MCP_DETAIL_JSONCONS_PP_QUOTE(,Member), \
/**/

#define MCP_DETAIL_JSONCONS_GENERATE_WNAME(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_GENERATE_WNAME_LAST(Prefix, P2, P3, Member, Count) 
#define MCP_DETAIL_JSONCONS_GENERATE_WNAME_LAST(Prefix, P2, P3, Member, Count) \
    MCP_DETAIL_JSONCONS_PP_QUOTE(L,Member), \
/**/

#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_LAST(Prefix, P2, P3, Member, Count) 
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_LAST(Prefix, P2, P3, Member, Count) \
    # Prefix "::" # Member, \
/**/

#define MCP_DETAIL_JSONCONS_N_MEMBER_IS(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_N_MEMBER_IS_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_IS_LAST(Prefix, P2, P3, Member, Count) if ((num_params-Count) < num_mandatory_params && !ajson.contains(object_names<value_type,char_type>::name(num_params-Count))) return false;

#define MCP_DETAIL_JSONCONS_N_MEMBER_AS(Prefix,P2,P3, Member, Count) MCP_DETAIL_JSONCONS_N_MEMBER_AS_LAST(Prefix,P2,P3, Member, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_AS_LAST(Prefix,P2,P3, Member, Count) { \
  auto it = ajson.find(object_names<value_type,char_type>::name(num_params-Count)); \
  if (it == ajson.object_range().end()) \
    {if ((num_params-Count) < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(num_params-Count));}} \
  else \
  { \
      auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(class_instance.Member)>::type>(aset, it->value()); \
      if (result) {set_member(std::move(*result), class_instance.Member);} \
      else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), error_context<value_type>::msg_arg(num_params-Count));} \
  } \
} \
/**/

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_AS(Prefix, P2,P3,Member, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_AS_LAST(Prefix,P2,P3, Member, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_AS_LAST(Prefix,P2,P3, Member, Count) { \
  auto it = ajson.find(object_names<value_type,char_type>::name(num_params-Count)); \
  if (it == ajson.object_range().end()) \
    {return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(num_params-Count));} \
  auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(class_instance.Member)>::type>(aset, it->value()); \
  if (result) {set_member(std::move(*result), class_instance.Member);} \
  else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), error_context<value_type>::msg_arg(num_params-Count));} \
} \
/**/

#define MCP_DETAIL_JSONCONS_TO_JSON(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_TO_JSON_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_TO_JSON_LAST(Prefix, P2, P3, Member, Count) if ((num_params-Count) < num_mandatory_params) \
    {ajson.try_emplace(object_names<value_type,char_type>::name(num_params-Count),class_instance.Member);} \
    else {json_traits_helper<Json>::set_optional_json_member(object_names<value_type,char_type>::name(num_params-Count),class_instance.Member, ajson);} \
/**/

#define MCP_DETAIL_JSONCONS_ALL_TO_JSON(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_ALL_TO_JSON_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_ALL_TO_JSON_LAST(Prefix, P2, P3, Member, Count) \
    ajson.try_emplace(object_names<value_type,char_type>::name(num_params-Count),class_instance.Member); \
/**/

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_DECODE(Prefix, P2,P3,Member, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_DECODE_LAST(Prefix, P2,P3,Member, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_DECODE_LAST(Prefix, P2,P3,Member, Count) \
    if (count++ >= num_params) { \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            if (!indices.all()) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(find_first_not_set(indices)), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
    } \
    else if (!indices[num_params-Count] && key == object_names<value_type,char_type>::name(num_params-Count)) { \
        cursor.next(ec); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        auto r1 = decode_traits<typename std::decay<decltype(val.Member)>::type>::decode(aset, cursor); \
        if (!r1) { \
            return result_type{mcp_detail_jsoncons::unexpect, r1.error()}; \
        } \
        set_member(std::move(*r1), val.Member); \
        indices[num_params-Count] = true; \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            if (!indices.all()) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(find_first_not_set(indices)), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
        key = get_key(cursor, ec); \
        if (ec) { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        count = 0; \
    } \
/**/

#define MCP_DETAIL_JSONCONS_N_MEMBER_DECODE(Prefix, P2,P3,Member, Count) MCP_DETAIL_JSONCONS_N_MEMBER_DECODE_LAST(Prefix, P2,P3,Member, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_DECODE_LAST(Prefix, P2,P3,Member, Count) \
    if (count++ >= num_params) { \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            std::size_t idx = find_first_not_set(indices); \
            if (idx < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
    } \
    else if (!indices[num_params-Count] && key == object_names<value_type,char_type>::name(num_params-Count)) { \
        cursor.next(ec); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        auto r1 = decode_traits<typename std::decay<decltype(val.Member)>::type>::decode(aset, cursor); \
        if (!r1) { \
            return result_type{mcp_detail_jsoncons::unexpect, r1.error()}; \
        } \
        set_member(std::move(*r1), val.Member); \
        indices[num_params-Count] = true; \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            std::size_t idx = find_first_not_set(indices); \
            if (idx < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
        key = get_key(cursor, ec); \
        if (ec) { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        count = 0; \
    } \
/**/

#define MCP_DETAIL_JSONCONS_N_MEMBER_ENCODE(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_N_MEMBER_ENCODE_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_ENCODE_LAST(Prefix, P2, P3, Member, Count) \
if ((num_params-Count) < num_mandatory_params) \
    { \
        auto r = try_encode_member(object_names<value_type,char_type>::name(num_params-Count), val.Member, encoder); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
    } \
    else \
    { \
        auto r = try_encode_optional_member(object_names<value_type,char_type>::name(num_params-Count), val.Member, encoder); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
    }

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_LAST(Prefix, P2, P3, Member, Count) \
    {auto r = try_encode_member(object_names<value_type,char_type>::name(num_params-Count), val.Member, encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;}} 

#define MCP_DETAIL_JSONCONS_MEMBER_COUNT(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_MEMBER_COUNT_LAST(Prefix, P2, P3, Member, Count)
#define MCP_DETAIL_JSONCONS_MEMBER_COUNT_LAST(Prefix, P2, P3, Member, Count) \
if ((num_params-Count) < num_mandatory_params) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Member)) \
    { \
        ++member_count; \
    } \
} 

#define MCP_DETAIL_JSONCONS_MEMBER_TRAITS_BASE(ToJson,Decode,Encode,NumTemplateParams,TypeName,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),char> \
    { \
        static string_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_NAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),wchar_t> \
    { \
        static wstring_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::wstring,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_WNAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct error_context<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        static std::string msg_arg(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> msg_args = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG,TypeName,,, __VA_ARGS__)\
            }; \
            return msg_args[index]; \
        } \
    }; \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_MEMBER_IS, ,,, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, # TypeName); \
            value_type class_instance = mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator()); \
            if (num_params == num_mandatory_params) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_ALL_MEMBER_AS,,,, __VA_ARGS__) \
            } \
            else \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_MEMBER_AS,,,, __VA_ARGS__) \
            } \
            return result_type(std::move(class_instance)); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(ToJson, ,,, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct decode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = read_result<value_type>; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static result_type decode(const allocator_set<Alloc,TempAlloc>& aset, basic_staj_cursor<CharT>& cursor) \
        { \
            using char_type = CharT; \
            value_type val{mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator())}; \
            std::error_code ec; \
            std::bitset<num_params> indices; \
            std::size_t count = 0; \
            if (cursor.current().event_type() != staj_events::begin_object) \
            { \
                return result_type{mcp_detail_jsoncons::unexpect, conv_errc::not_map, cursor.line(), cursor.column()}; \
            } \
            bool is_end = read_next_or_end(cursor, ec); \
            if (ec) \
            { \
                return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
            } \
            if (is_end) \
            { \
            std::size_t idx = find_first_not_set(indices); \
            if (idx < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
            } \
            auto key = get_key(cursor, ec); \
            if (ec) { \
                return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
            } \
            while (true) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(Decode, ,,, __VA_ARGS__) \
            } \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            std::error_code ec; \
            using char_type = CharT; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_MEMBER_COUNT, ,,, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(Encode, ,,, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/

#define MCP_DETAIL_JSONCONS_N_MEMBER_TRAITS(TypeName,NumMandatoryParams,...)  \
    MCP_DETAIL_JSONCONS_MEMBER_TRAITS_BASE(MCP_DETAIL_JSONCONS_TO_JSON, MCP_DETAIL_JSONCONS_N_MEMBER_DECODE, MCP_DETAIL_JSONCONS_N_MEMBER_ENCODE, 0, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_N_MEMBER_TRAITS(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_TRAITS_BASE(MCP_DETAIL_JSONCONS_TO_JSON, MCP_DETAIL_JSONCONS_N_MEMBER_DECODE, MCP_DETAIL_JSONCONS_N_MEMBER_ENCODE,NumTemplateParams, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_TRAITS(TypeName, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_TO_JSON, MCP_DETAIL_JSONCONS_ALL_MEMBER_DECODE, MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE,0,TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__), __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_ALL_MEMBER_TRAITS(NumTemplateParams, TypeName, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_TO_JSON, MCP_DETAIL_JSONCONS_ALL_MEMBER_DECODE, MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE,NumTemplateParams,TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__), __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/ 

#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params && MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_2(Member, Name) !ajson.contains(Name)) return false;
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_2(Member, Name)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_6(Member, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_IS_6(Member, Name, Mode, Match, Into, From) !ajson.contains(Name)) return false; \
    MCP_DETAIL_JSONCONS_TRY{if (!Match(ajson.at(Name).template as<typename std::decay<decltype(Into((std::declval<value_type*>())->Member))>::type>())) return false;} \
    MCP_DETAIL_JSONCONS_CATCH(...) {return false;}

#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_2(Member, Name) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)  
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_3(Member, Name, Mode) Mode(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name, Mode,always_true(),,))
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_6(Member, Name, Mode, Match, Into, From) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into, From)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into, From) { \
  auto it = ajson.find(Name); \
  if (it == ajson.object_range().end()) \
    {if (index < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(index));}} \
  else \
  { \
    auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(Into(class_instance.Member))>::type>(aset, it->value()); \
    if (result && !Match(From(* result))) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, error_context<value_type>::msg_arg(index));} \
    Mode(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_8(Member, Name, Mode, Match, Into, From)) }}
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS_8(Member, Name, Mode, Match, Into, From) \
    if (result) { \
      set_member(From(std::move(*result)), class_instance.Member); \
    } \
    else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), class_name);} 
  
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_2(Member, Name) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)  
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_3(Member, Name, Mode) Mode(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name,Mode,always_true(),,))
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_6(Member, Name, Mode, Match, Into, From) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into, From)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_7(Member, Name, Mode, Match, Into, From) { \
  auto it = ajson.find(Name); \
  if (it == ajson.object_range().end()) \
    {return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(index));} \
  auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(Into(class_instance.Member))>::type>(aset, it->value()); \
  if (result && !Match(From(* result))) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, error_context<value_type>::msg_arg(index));} \
  Mode(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_8(Member, Name, Mode, Match, Into, From)) }

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS_8(Member, Name, Mode, Match, Into, From) \
  if (result) { \
    set_member(From(std::move(*result)), class_instance.Member); \
  } \
  else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), class_name);} 

#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_2(Member, Name) \
  {ajson.try_emplace(Name,class_instance.Member);} \
else \
  {json_traits_helper<Json>::set_optional_json_member(string_view_type(Name),class_instance.Member, ajson);}
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_2(Member, Name)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match, Into, From) \
  {ajson.try_emplace(Name, Into(class_instance.Member));} \
else \
  {json_traits_helper<Json>::set_optional_json_member(string_view_type(Name), Into(class_instance.Member), ajson);}

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_2(Member, Name) ajson.try_emplace(Name,class_instance.Member);
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_2(Member, Name)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON_6(Member, Name, Mode, Match, Into, From) ajson.try_emplace(Name, Into(class_instance.Member));

#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_2(Member, Name) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_6(Member, Name,,,,)
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_6(Member, Name,Mode,,,)
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME_6(Member, Name, Mode, Match, Into, From) generate_msg_arg(class_name, # Member),

#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_2(Member, Name) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)  
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name, Mode,always_true(),,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_6(Member, Name, Mode, Match, Into, From) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into, From)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into, From) \
    if (count++ >= num_params) { \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            std::size_t idx = find_first_not_set(indices); \
            if (idx < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
    } \
    else if (!indices[index] && key == Name) { \
        cursor.next(ec); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        auto r1 = decode_traits<typename std::decay<decltype(Into(val.Member))>::type>::decode(aset, cursor); \
        if (!r1) { \
            return result_type{mcp_detail_jsoncons::unexpect, r1.error()}; \
        } \
        if (!Match(*r1)) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, error_context<value_type>::msg_arg(index));} \
        Mode(set_member(From(std::move(*r1)), val.Member)); \
        indices[index] = true; \
        is_end = read_next_or_end(cursor, ec); \
        if (ec) \
        { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        if (is_end) \
        { \
            std::size_t idx = find_first_not_set(indices); \
            if (idx < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
        } \
        key = get_key(cursor, ec); \
        if (ec) { \
            return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
        } \
        count = 0; \
    } \
/**/

#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_2(Member, Name) \
    { \
        auto r = try_encode_member(string_view_type(Name), val.Member, encoder); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
    } \
    else \
    { \
        auto r = try_encode_optional_member(string_view_type(Name), val.Member, encoder); \
        if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
    }    
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_2(Member, Name)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE_6(Member, Name, Mode, Match, Into, From) \
{ \
    auto r = try_encode_member(string_view_type(Name), Into(val.Member), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(string_view_type(Name), Into(val.Member), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
}    


#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_2(Member, Name) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)  
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name,Mode,always_true(),,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_6(Member, Name, Mode, Match, Into, From) MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into, From)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE_7(Member, Name, Mode, Match, Into, From) \
   if (count++ >= num_params) { \
       is_end = read_next_or_end(cursor, ec); \
       if (ec) \
       { \
           return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
       } \
       if (is_end) \
       { \
           std::size_t idx = find_first_not_set(indices); \
           if (idx < num_mandatory_params) { \
               return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                   cursor.line(), cursor.column()); \
           } \
           return result_type{std::move(val)}; \
       } \
   } \
   else if (!indices[index] && key == Name) { \
       cursor.next(ec); \
       if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) \
       { \
           return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
       } \
       auto r1 = decode_traits<typename std::decay<decltype(Into(val.Member))>::type>::decode(aset, cursor); \
       if (!r1) { \
           return result_type{mcp_detail_jsoncons::unexpect, r1.error()}; \
       } \
       if (!Match(*r1)) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, error_context<value_type>::msg_arg(index));} \
       Mode(set_member(From(std::move(*r1)), val.Member)); \
       indices[index] = true; \
       is_end = read_next_or_end(cursor, ec); \
       if (ec) \
       { \
           return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
       } \
       if (is_end) \
       { \
           std::size_t idx = find_first_not_set(indices); \
           if (idx < num_mandatory_params) { \
               return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(idx), \
                   cursor.line(), cursor.column()); \
           } \
           return result_type{std::move(val)}; \
       } \
       key = get_key(cursor, ec); \
       if (ec) { \
           return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
       } \
       count = 0; \
   } \
/**/

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_2(Member, Name) \
     {auto r = try_encode_member(string_view_type(Name), val.Member, encoder); if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;}} 
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_2(Member, Name)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME_6(Member, Name, Mode, Match, Into, From) \
    {auto r = try_encode_member(string_view_type(Name), Into(val.Member), encoder); if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;}} 

#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_2(Member, Name) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Member)) \
    { \
        ++member_count; \
    } \
}    
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_3(Member, Name, Mode) MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_2(Member, Name)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_4(Member, Name, Mode, Match) MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_6(Member, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_5(Member, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_6(Member, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT_6(Member, Name, Mode, Match, Into, From) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Member)) \
    { \
        ++member_count; \
    } \
}    

#define MCP_DETAIL_JSONCONS_MEMBER_NAME_TRAITS_BASE(ToJson,Decode,Encode, NumTemplateParams,TypeName,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct error_context<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        static std::string msg_arg(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static constexpr const char* class_name = # TypeName; \
            static std::array<std::string,num_params> msg_args = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG_NAME,TypeName,,, __VA_ARGS__)\
            }; \
            return msg_args[index]; \
        } \
    }; \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_MEMBER_NAME_IS,,,, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            static const char* class_name = # TypeName; \
            std::error_code ec; \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, # TypeName); \
            value_type class_instance = mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator()); \
            std::size_t index = 0; \
            if (num_params == num_mandatory_params) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_AS,,,, __VA_ARGS__) \
            } \
            else \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_AS,,,, __VA_ARGS__) \
            } \
            return result_type(std::move(class_instance)); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(ToJson,,,, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct decode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = read_result<value_type>; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static result_type decode(const allocator_set<Alloc,TempAlloc>& aset, basic_staj_cursor<CharT>& cursor) \
        { \
            value_type val{mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator())}; \
            std::error_code ec; \
            std::bitset<num_params> indices; \
            std::size_t count = 0; \
            if (cursor.current().event_type() != staj_events::begin_object) \
            { \
                return result_type{mcp_detail_jsoncons::unexpect, conv_errc::not_map, cursor.line(), cursor.column()}; \
            } \
            bool is_end = read_next_or_end(cursor, ec); \
            if (ec) \
            { \
                return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
            } \
            if (is_end) \
            { \
            std::size_t index = find_first_not_set(indices); \
            if (index < num_mandatory_params) { \
                return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(index), \
                    cursor.line(), cursor.column()); \
            } \
            return result_type{std::move(val)}; \
            } \
            auto key = get_key(cursor, ec); \
            if (ec) { \
                return result_type{mcp_detail_jsoncons::unexpect, ec, cursor.line(), cursor.column()}; \
            } \
            std::size_t index = 0; \
            while (true) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(Decode, ,,, __VA_ARGS__) \
            } \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::error_code ec; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_MEMBER_NAME_COUNT, ,,, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(Encode, ,,, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/


#define MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TRAITS(TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON, MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE, MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE, 0, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_N_MEMBER_NAME_TRAITS(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_MEMBER_NAME_TO_JSON, MCP_DETAIL_JSONCONS_N_MEMBER_NAME_DECODE, MCP_DETAIL_JSONCONS_N_MEMBER_NAME_ENCODE, NumTemplateParams, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TRAITS(TypeName, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON, MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE, MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME, 0, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_ALL_MEMBER_NAME_TRAITS(NumTemplateParams, TypeName, ...)  \
    MCP_DETAIL_JSONCONS_MEMBER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_TO_JSON, MCP_DETAIL_JSONCONS_ALL_MEMBER_NAME_DECODE, MCP_DETAIL_JSONCONS_ALL_MEMBER_ENCODE_NAME, NumTemplateParams, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_IS(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_IS_LAST(Prefix, P2, P3, Getter, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_IS_LAST(Prefix, P2, P3, Getter, Count) if ((num_params-Count) < num_mandatory_params && !ajson.contains(object_names<value_type,char_type>::name(num_params-Count))) return false;

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_GET(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_GET_LAST(Prefix, P2, P3, Getter, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_GET_LAST(Prefix, P2, P3, Getter, Count) \
  conversion_result<typename std::decay<decltype((std::declval<value_type*>())->Getter())>::type> _r ## Getter{unexpect, conv_errc::missing_required_member}; \
  {auto it = ajson.find(object_names<value_type,char_type>::name(num_params-Count)); \
  if (it == ajson.object_range().end()) \
    {if ((num_params-Count) < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, error_context<value_type>::msg_arg(num_params-Count));}} \
  else \
  { \
    _r ## Getter = json_traits_helper<Json>::template try_as_value<typename decltype(_r ## Getter)::value_type>(aset, it->value()); \
    if (!_r ## Getter) {return result_type(mcp_detail_jsoncons::unexpect, _r ## Getter.error().code(), error_context<value_type>::msg_arg(num_params-Count));} \
  }} 

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_AS(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_AS_LAST(Prefix, P2, P3, Getter, Count),
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_AS_LAST(Prefix, P2, P3, Getter, Count) \
  _r ## Getter ? std::move(*_r ## Getter) : mcp_detail_jsoncons::make_obj_using_allocator<typename std::decay<decltype((std::declval<value_type*>())->Getter())>::type>(aset.get_allocator())

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_TO_JSON(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_TO_JSON_LAST(Prefix, P2, P3, Getter, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_TO_JSON_LAST(Prefix, P2, P3, Getter, Count) \
if ((num_params-Count) < num_mandatory_params) { \
       ajson.try_emplace(object_names<value_type,char_type>::name(num_params-Count),class_instance.Getter()); \
  } \
else { \
  json_traits_helper<Json>::set_optional_json_member(object_names<value_type,char_type>::name(num_params-Count),class_instance.Getter(), ajson); \
}

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_COUNT(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_COUNT_LAST(Prefix, P2, P3, Getter, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_COUNT_LAST(Prefix, P2, P3, Getter, Count) \
if ((num_params-Count) < num_mandatory_params) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
} 

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_ENCODE(Prefix, P2, P3, Getter, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_ENCODE_LAST(Prefix, P2, P3, Getter, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_ENCODE_LAST(Prefix, P2, P3, Getter, Count) \
if ((num_params-Count) < num_mandatory_params) \
{ \
    auto r = try_encode_member(object_names<value_type,char_type>::name(num_params-Count), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(object_names<value_type,char_type>::name(num_params-Count), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} 

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_TRAITS_BASE(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),char> \
    { \
        static string_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_NAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),wchar_t> \
    { \
        static wstring_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::wstring,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_WNAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct error_context<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        static std::string msg_arg(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> msg_args = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG,TypeName,,, __VA_ARGS__)\
            }; \
            return msg_args[index]; \
        } \
    }; \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_IS, ,,, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, # TypeName); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_GET,TypeName,,, __VA_ARGS__) \
            return result_type(mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator(), MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_AS, ,,, __VA_ARGS__) )); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_TO_JSON, ,,, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::error_code ec; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_COUNT, ,,, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_ENCODE, ,,, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/
 
#define MCP_DETAIL_JSONCONS_ALL_CTOR_GETTER_TRAITS(TypeName, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_TRAITS_BASE(0, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_TPL_ALL_CTOR_GETTER_TRAITS(NumTemplateParams, TypeName, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_TRAITS_BASE(NumTemplateParams, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_N_CTOR_GETTER_TRAITS(TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_TRAITS_BASE(0, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_N_ALL_CTOR_GETTER_TRAITS(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_TRAITS_BASE(NumTemplateParams, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params && MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_2(Getter, Name) !ajson.contains(Name)) return false;
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_3(Getter, Name, Mode) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_4(Getter, Name, Mode, Match) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_6(Getter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_5(Getter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_6(Getter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS_6(Getter, Name, Mode, Match, Into, From) !ajson.contains(Name)) return false; \
    MCP_DETAIL_JSONCONS_TRY{if (!Match(ajson.at(Name).template as<typename std::decay<decltype(Into((std::declval<value_type*>())->Getter()))>::type>())) return false;} \
    MCP_DETAIL_JSONCONS_CATCH(...) {return false;}
 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_2(Getter, Name) 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_3(Getter, Name, Mode) 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_4(Getter, Name, Mode, Match) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_6(Getter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_5(Getter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_6(Getter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH_6(Getter, Name, Mode, Match, Into, From) { \
  auto it = ajson.find(Name); \
  if (it != ajson.object_range().end()) \
  { \
    auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(Into((std::declval<value_type*>())->Getter()))>::type>(aset, it->value()); \
    if (result && !Match(* result)) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, class_name);} \
  } \
}

#define MCP_DETAIL_JSONCONS_COMMA ,

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_2(Getter, Name) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_3(Getter, Name, Mode) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name, Mode, always_true(),,))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_4(Getter, Name, Mode, Match) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name, Mode, Match,,))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_5(Getter, Name, Mode, Match, Into) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name, Mode, Match, Into,))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_6(Getter, Name, Mode, Match, Into, From) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name, Mode, Match, Into, From))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET_7(Getter, Name, Mode, Match, Into, From) \
  conversion_result<typename std::decay<decltype(Into((std::declval<value_type*>())->Getter()))>::type> _r ## Getter{unexpect, conv_errc::missing_required_member}; \
  {auto it = ajson.find(Name); \
  if (it == ajson.object_range().end()) \
    {if (index < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, class_name);}} \
  else \
  { \
    _r ## Getter = json_traits_helper<Json>::template try_as_value<typename decltype(_r ## Getter)::value_type>(aset, it->value()); \
    if (!_r ## Getter) {return result_type(mcp_detail_jsoncons::unexpect, _r ## Getter.error().code(), class_name);} \
    if (!Match(* _r ## Getter)) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, class_name);} \
  }} 

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_2(Getter, Name) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name,,,, ) MCP_DETAIL_JSONCONS_COMMA
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_3(Getter, Name, Mode) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name, Mode,,, ) MCP_DETAIL_JSONCONS_COMMA) 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_4(Getter, Name, Mode, Match) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name, Mode, Match,, ) MCP_DETAIL_JSONCONS_COMMA) 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_5(Getter, Name, Mode, Match, Into) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name, Mode, Match, Into, ) MCP_DETAIL_JSONCONS_COMMA) 
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_6(Getter, Name, Mode, Match, Into, From) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter,Name,Mode,Match,Into,From) MCP_DETAIL_JSONCONS_COMMA)

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_2(Getter, Name) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name,,,,)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_3(Getter, Name, Mode) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name,Mode,,,))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_4(Getter, Name, Mode, Match) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_6(Getter, Name, Mode, Match,,))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_5(Getter, Name, Mode, Match, Into) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_6(Getter, Name, Mode, Match, Into, ))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_6(Getter, Name, Mode, Match, Into, From) Mode(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name, Mode, Match, Into, From))
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS_LAST_7(Getter, Name, Mode, Match, Into, From) \
  _r ## Getter ? From(std::move(*_r ## Getter)) : From(mcp_detail_jsoncons::make_obj_using_allocator<typename std::decay<decltype(Into((std::declval<value_type*>())->Getter()))>::type>(aset.get_allocator()))

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_2(Getter, Name) \
{ \
  ajson.try_emplace(Name,class_instance.Getter()); \
} \
else { \
  json_traits_helper<Json>::set_optional_json_member(string_view_type(Name),class_instance.Getter(), ajson); \
}
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_3(Getter, Name, Mode) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_4(Getter, Name, Mode, Match) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_5(Getter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_6(Getter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON_6(Getter, Name, Mode, Match, Into, From) \
{ \
  ajson.try_emplace(Name, Into(class_instance.Getter()) ); \
} \
else { \
  json_traits_helper<Json>::set_optional_json_member(string_view_type(Name), Into(class_instance.Getter()), ajson); \
}

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_2(Getter, Name) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
}    
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_3(Getter, Name, Mode) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_4(Getter, Name, Mode, Match) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_5(Getter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_6(Getter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT_6(Getter, Name, Mode, Match, Into, From) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
}    

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_2(Getter, Name) \
{ \
    auto r = try_encode_member(string_view_type(Name), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(string_view_type(Name), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
}
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_3(Getter, Name, Mode) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_4(Getter, Name, Mode, Match) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_2(Getter, Name)
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_5(Getter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_6(Getter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE_6(Getter, Name, Mode, Match, Into, From) \
{ \
    auto r = try_encode_member(string_view_type(Name), Into(val.Getter()), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(string_view_type(Name), Into(val.Getter()), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
}

#define MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TRAITS_BASE(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_IS,,,, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            const char* class_name = # TypeName; \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, # TypeName); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_MATCH,,,, __VA_ARGS__)\
            std::size_t index = 0; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_GET,TypeName,,, __VA_ARGS__) \
            return result_type(mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator(), MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_AS,,,, __VA_ARGS__))); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TO_JSON,,,, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::error_code ec; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_COUNT,,,, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_ENCODE,,,, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/
                                                                       
#define MCP_DETAIL_JSONCONS_ALL_CTOR_GETTER_NAME_TRAITS(TypeName, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TRAITS_BASE(0, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_TPL_ALL_CTOR_GETTER_NAME_TRAITS(NumTemplateParams, TypeName, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TRAITS_BASE(NumTemplateParams, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_N_CTOR_GETTER_NAME_TRAITS(TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TRAITS_BASE(0, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_TPL_N_CTOR_GETTER_NAME_TRAITS(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
MCP_DETAIL_JSONCONS_CTOR_GETTER_NAME_TRAITS_BASE(NumTemplateParams, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_ENUM_PAIR(Prefix, P2, P3, Member, Count) MCP_DETAIL_JSONCONS_ENUM_PAIR_LAST(Prefix, P2, P3, Member, Count),
#define MCP_DETAIL_JSONCONS_ENUM_PAIR_LAST(Prefix, P2, P3, Member, Count) {value_type::Member, object_names<value_type,char_type>::name(num_params-Count)}

#define MCP_DETAIL_JSONCONS_ENUM_TRAITS_BASE(EnumType, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template<> \
    struct object_names<EnumType,char> \
    { \
        static string_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_NAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template<> \
    struct object_names<EnumType,wchar_t> \
    { \
        static wstring_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::wstring,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_WNAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template<> \
    struct error_context<EnumType> \
    { \
        static std::string msg_arg(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> msg_args = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG,TypeName,,, __VA_ARGS__)\
            }; \
            return msg_args[index]; \
        } \
    }; \
    template<> \
    struct reflect_type_properties<EnumType> \
    { \
        using value_type = EnumType; \
        static constexpr std::size_t count = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        template <typename CharT> \
        static const std::pair<EnumType,basic_string_view<CharT>>* values() \
        { \
            using char_type = CharT; \
            static const std::pair<EnumType,basic_string_view<CharT>> values[] = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_ENUM_PAIR, ,,, __VA_ARGS__)\
            };\
            return values; \
        } \
    }; \
    template <typename Json> \
    struct json_traits<Json, EnumType> \
    { \
        static_assert(std::is_enum<EnumType>::value, # EnumType " must be an enum"); \
        using value_type = EnumType; \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_type = std::basic_string<char_type>; \
        using string_view_type = basic_string_view<char_type>; \
        using mapped_type = std::pair<value_type,string_view_type>; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_string()) return false; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            auto rs = ajson.try_as_string_view(); \
            if (!rs) return false; \
            const string_view_type s = *rs; \
            if (s.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return true; \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == s; }); \
            return it != last; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& /*aset*/, const Json& ajson) \
        { \
            if (!is(ajson)) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType); \
            auto rs = ajson.try_as_string_view(); \
            if (!rs) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType); \
            const string_view_type s = *rs; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            if (s.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return value_type(); \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == s; }); \
            if (it == last) \
            { \
                if (s.empty()) \
                { \
                    return result_type(value_type()); \
                } \
                else \
                { \
                    return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType); \
                } \
            } \
            return result_type((*it).first); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, value_type class_instance) \
        { \
            static constexpr char_type empty_string[] = {0}; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            auto it = std::find_if(first, last, \
                                   [class_instance](const mapped_type& item) -> bool \
                                   { return item.first == class_instance; }); \
            if (it == last) \
            { \
                if (class_instance == value_type()) \
                { \
                    return Json(empty_string); \
                } \
                else \
                { \
                    MCP_DETAIL_JSONCONS_THROW(conv_error(conv_errc::conversion_failed, # EnumType)); \
                } \
            } \
            return mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), (*it).second, semantic_tag::none); \
        } \
    }; \
    template <> struct encode_traits<EnumType> \
    { \
        using value_type = EnumType; \
        using result_type = conversion_result<value_type>; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            using mapped_type = std::pair<value_type,string_view_type>; \
            static const char_type empty_string[] = {0}; \
            std::error_code ec; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            auto it = std::find_if(first, last, \
                                   [val](const mapped_type& item) -> bool \
                                   { return item.first == val; }); \
            if (it == last) \
            { \
                if (val == value_type()) \
                { \
                    encoder.string_value(empty_string, semantic_tag::none, ser_context(), ec); \
                    if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
                    return write_result{}; \
                } \
                else \
                { \
                    return write_result{unexpect, conv_errc::conversion_failed}; \
                } \
            } \
            encoder.string_value((*it).second, semantic_tag::none, ser_context(), ec); \
            return write_result{}; \
        } \
    }; \
    template <> struct decode_traits<EnumType> \
    { \
        using value_type = EnumType; \
        using result_type = read_result<value_type>; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static result_type decode(const allocator_set<Alloc,TempAlloc>&, basic_staj_cursor<CharT>& cursor) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            using mapped_type = std::pair<value_type,string_view_type>; \
            std::error_code ec; \
            auto sv = cursor.current().template get<string_view_type>(ec); \
            if (ec) \
            { \
                return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType, cursor.line(), cursor.column()); \
            } \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            if (sv.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return value_type(); \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == sv; }); \
            if (it == last) \
            { \
                if (sv.empty()) \
                { \
                    return result_type(value_type()); \
                } \
                else \
                { \
                    return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType, cursor.line(), cursor.column()); \
                } \
            } \
            return result_type((*it).first); \
        } \
    }; \
} \
} \
    /**/

#define MCP_DETAIL_JSONCONS_ENUM_TRAITS(EnumType, ...)  \
    MCP_DETAIL_JSONCONS_ENUM_TRAITS_BASE(EnumType,__VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<EnumType> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR_ Seq),
#define MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR_ Seq)
#define MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR_(Member, Name) {value_type::Member, Name}

#define MCP_DETAIL_JSONCONS_ENUM_NAME_TRAITS(EnumType, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template<> \
    struct reflect_type_properties<EnumType> \
    { \
        using value_type = EnumType; \
        static constexpr std::size_t count = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        template <typename CharT> \
        static const std::pair<EnumType,basic_string_view<CharT>>* values() \
        { \
            static const std::pair<EnumType,basic_string_view<CharT>> values[] = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_NAME_ENUM_PAIR, ,,, __VA_ARGS__)\
            };\
            return values; \
        } \
    }; \
    template <typename Json> \
    struct json_traits<Json, EnumType> \
    { \
        static_assert(std::is_enum<EnumType>::value, # EnumType " must be an enum"); \
        using value_type = EnumType; \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_type = std::basic_string<char_type>; \
        using string_view_type = basic_string_view<char_type>; \
        using mapped_type = std::pair<value_type,string_view_type>; \
        \
        static bool is(const Json& ajson) noexcept \
        { \
            auto rs = ajson.try_as_string_view(); \
            if (!rs) {return false;} \
            const string_view_type s = *rs; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            if (s.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return true; \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == s; }); \
            return it != last; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& /*aset*/, const Json& ajson) \
        { \
            auto rs = ajson.try_as_string_view(); \
            if (!rs) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType);} \
            const string_view_type s = *rs; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            if (s.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return result_type(value_type()); \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == s; }); \
            if (it == last) \
            { \
                if (s.empty()) \
                { \
                    return result_type(value_type()); \
                } \
                else \
                { \
                    return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType); \
                } \
            } \
            return (*it).first; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, value_type class_instance) \
        { \
            static constexpr char_type empty_string[] = {0}; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            auto it = std::find_if(first, last, \
                                   [class_instance](const mapped_type& item) -> bool \
                                   { return item.first == class_instance; }); \
            if (it == last) \
            { \
                if (class_instance == value_type()) \
                { \
                    return Json(empty_string); \
                } \
                else \
                { \
                    MCP_DETAIL_JSONCONS_THROW(conv_error(conv_errc::conversion_failed, # EnumType)); \
                } \
            } \
            return mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), (*it).second, semantic_tag::none); \
        } \
    }; \
    template <> struct encode_traits<EnumType> \
    { \
        using value_type = EnumType; \
        using result_type = conversion_result<value_type>; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            using mapped_type = std::pair<value_type,string_view_type>; \
            static const char_type empty_string[] = {0}; \
            std::error_code ec; \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            auto it = std::find_if(first, last, \
                                   [val](const mapped_type& item) -> bool \
                                   { return item.first == val; }); \
            if (it == last) \
            { \
                if (val == value_type()) \
                { \
                    encoder.string_value(empty_string, semantic_tag::none, ser_context(), ec); \
                    if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) return write_result{unexpect, ec}; \
                    return write_result{}; \
                } \
                else \
                { \
                    return write_result{unexpect, conv_errc::conversion_failed}; \
                } \
            } \
            encoder.string_value((*it).second, semantic_tag::none, ser_context(), ec); \
            return write_result{}; \
        } \
    }; \
    template <> struct decode_traits<EnumType> \
    { \
        using value_type = EnumType; \
        using result_type = read_result<value_type>; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static result_type decode(const allocator_set<Alloc,TempAlloc>&, basic_staj_cursor<CharT>& cursor) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            using mapped_type = std::pair<value_type,string_view_type>; \
            std::error_code ec; \
            auto sv = cursor.current().template get<string_view_type>(ec); \
            if (ec) \
            { \
                return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType, cursor.line(), cursor.column()); \
            } \
            auto first = reflect_type_properties<value_type>::values<char_type>(); \
            auto last = first + reflect_type_properties<value_type>::count; \
            if (sv.empty() && std::find_if(first, last, \
                                          [](const mapped_type& item) -> bool \
                                          { return item.first == value_type(); }) == last) \
            { \
                return value_type(); \
            } \
            auto it = std::find_if(first, last, \
                                   [&](const mapped_type& item) -> bool \
                                   { return item.second == sv; }); \
            if (it == last) \
            { \
                if (sv.empty()) \
                { \
                    return result_type(value_type()); \
                } \
                else \
                { \
                    return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed, # EnumType, cursor.line(), cursor.column()); \
                } \
            } \
            return result_type((*it).first); \
        } \
    }; \
} \
    template <> struct is_json_type_traits_declared<EnumType> : public std::true_type {}; \
} \
    /**/

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count)  
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS_(Prefix, Getter, Setter, Property, Count) { \
  auto it = ajson.find(object_names<value_type,char_type>::name(num_params-Count)); \
  if (it == ajson.object_range().end()) \
    {if ((num_params-Count) < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, # Prefix);}} \
  else \
  { \
    auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(class_instance.Getter())>::type>(aset, it->value()); \
    if (result) {class_instance.Setter(std::move(* result));} \
    else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), # Prefix);} \
  } \
}

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS_(Prefix, Getter, Setter, Property, Count) { \
  auto it = ajson.find(object_names<value_type,char_type>::name(num_params-Count)); \
  if (it == ajson.object_range().end()) \
    {return result_type(unexpect, conv_errc::missing_required_member, # Prefix "::" # Property);} \
  auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(class_instance.Getter())>::type>(aset, it->value()); \
  if (!result) {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), # Prefix "::" # Property);} \
  class_instance.Setter(std::move(* result)); \
}

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON_(Prefix, Getter, Setter, Property, Count) \
if ((num_params-Count) < num_mandatory_params) \
  {ajson.try_emplace(object_names<value_type,char_type>::name(num_params-Count),class_instance.Getter());} \
else \
  {json_traits_helper<Json>::set_optional_json_member(object_names<value_type,char_type>::name(num_params-Count),class_instance.Getter(), ajson);}

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON_(Prefix, Getter, Setter, Property, Count) ajson.try_emplace(object_names<value_type,char_type>::name(num_params-Count),class_instance.Getter());

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT_(Prefix, Getter, Setter, Property, Count) \
if ((num_params-Count) < num_mandatory_params) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
} 

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE_LAST(Prefix, GetPrefix, SetPrefix, Property, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE_(Prefix, GetPrefix ## Property, SetPrefix ## Property, Property, Count) 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE_(Prefix, Getter, Setter, Property, Count) \
if ((num_params-Count) < num_mandatory_params) \
{ \
    auto r = try_encode_member(object_names<value_type,char_type>::name(num_params-Count), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(object_names<value_type,char_type>::name(num_params-Count), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} 

#define MCP_DETAIL_JSONCONS_GETTER_SETTER_TRAITS_BASE(ToJson,NumTemplateParams, TypeName,GetPrefix,SetPrefix,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),char> \
    { \
        static string_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_NAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct object_names<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams),wchar_t> \
    { \
        static wstring_view name(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::wstring,num_params> names = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_WNAME,TypeName,,, __VA_ARGS__)\
            }; \
            return names[index]; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct error_context<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        static std::string msg_arg(std::size_t index) \
        { \
            static constexpr std::size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
            static std::array<std::string,num_params> msg_args = { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GENERATE_MSG_ARG,TypeName,,, __VA_ARGS__)\
            }; \
            return msg_args[index]; \
        } \
    }; \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_MEMBER_IS, ,GetPrefix,SetPrefix, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, # TypeName); \
            value_type class_instance = mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator()); \
            if (num_params == num_mandatory_params) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_AS,TypeName,GetPrefix,SetPrefix, __VA_ARGS__) \
            } \
            else \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_AS,TypeName,GetPrefix,SetPrefix, __VA_ARGS__) \
            } \
            return result_type(std::move(class_instance)); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(ToJson, ,GetPrefix,SetPrefix, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::error_code ec; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_COUNT, ,GetPrefix,SetPrefix, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_ENCODE, ,GetPrefix,SetPrefix, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TRAITS(TypeName,GetPrefix,SetPrefix,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON,0, TypeName,GetPrefix,SetPrefix,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_N_GETTER_SETTER_TRAITS(NumTemplateParams, TypeName,GetPrefix,SetPrefix,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_TO_JSON,NumTemplateParams, TypeName,GetPrefix,SetPrefix,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TRAITS(TypeName,GetPrefix,SetPrefix, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON,0,TypeName,GetPrefix,SetPrefix, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__), __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_TPL_ALL_GETTER_SETTER_TRAITS(NumTemplateParams, TypeName,GetPrefix,SetPrefix, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_TO_JSON,NumTemplateParams,TypeName,GetPrefix,SetPrefix, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__), __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params && MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_3(Getter, Setter, Name) !ajson.contains(Name)) return false;
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_7(Getter, Setter, Name, Mode, Match,, )
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS_7(Getter, Setter, Name, Mode, Match, Into, From) !ajson.contains(Name)) return false; \
    MCP_DETAIL_JSONCONS_TRY{if (!Match(ajson.at(Name).template as<typename std::decay<decltype(Into((std::declval<value_type*>())->Getter()))>::type>())) return false;} \
    MCP_DETAIL_JSONCONS_CATCH(...) {return false;}

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_LAST(P1, P2, P3, Seq, Count) index = num_params-Count; MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_3(Getter, Setter, Name) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name,MCP_DETAIL_JSONCONS_RDWR,always_true(),,)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_4(Getter, Setter, Name, Mode) Mode(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, always_true(),,))
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match, Into, From) { \
  auto it = ajson.find(Name); \
  if (it == ajson.object_range().end()) \
    {if (index < num_mandatory_params){return result_type(unexpect, conv_errc::missing_required_member, class_name);}} \
  else \
  { \
    auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(Into(class_instance.Getter()))>::type>(aset, it->value()); \
    if (result && !Match(From(* result))) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, class_name);} \
    Mode(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_8(Getter, Setter, Name, Mode, Match, Into, From)) }}

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS_8(Getter, Setter, Name, Mode, Match, Into, From) \
  if (result) { \
    class_instance.Setter(From(std::move(* result))); \
  } \
  else if (index < num_mandatory_params) {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), class_name);} \
  else if (result.error().code() != conv_errc::missing_required_member){return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), class_name);} 

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_3(Getter, Setter, Name) \
    ajson.try_emplace(Name,class_instance.Getter());
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, Into, From) \
    ajson.try_emplace(Name, Into(class_instance.Getter()) );

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_LAST(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_3(Getter, Setter, Name) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name,MCP_DETAIL_JSONCONS_RDWR, always_true(),,)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_4(Getter, Setter, Name, Mode) Mode(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name,Mode, always_true(),,))
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match,,)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match, Into,)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_7(Getter, Setter, Name, Mode, Match, Into, From) { \
  auto it = ajson.find(Name); \
  if (it == ajson.object_range().end()) \
    {return result_type(unexpect, conv_errc::missing_required_member, class_name);} \
  auto result = json_traits_helper<Json>::template try_as_value<typename std::decay<decltype(Into(class_instance.Getter()))>::type>(aset, it->value()); \
  if (result && !Match(From(* result))) {return result_type(mcp_detail_jsoncons::unexpect, conv_errc::match_failed, class_name);} \
  Mode(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_8(Getter, Setter, Name, Mode, Match, Into, From)) }

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS_8(Getter, Setter, Name, Mode, Match, Into, From) \
  if (result) { \
    class_instance.Setter(From(std::move(* result))); \
  } \
  else {return result_type(mcp_detail_jsoncons::unexpect, result.error().code(), class_name);} 

#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_3(Getter, Setter, Name) \
  ajson.try_emplace(Name,class_instance.Getter()); \
else \
  {json_traits_helper<Json>::set_optional_json_member(string_view_type(Name),class_instance.Getter(), ajson);}
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON_7(Getter, Setter, Name, Mode, Match, Into, From) \
  ajson.try_emplace(Name, Into(class_instance.Getter())); \
else \
  {json_traits_helper<Json>::set_optional_json_member(string_view_type(Name), Into(class_instance.Getter()), ajson);}
 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_3(Getter, Setter, Name) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
}    
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_7(Getter, Setter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT_7(Getter, Setter, Name, Mode, Match, Into, From) \
{ \
    ++member_count; \
} \
else \
{ \
    if (is_optional_value_set(val.Getter())) \
    { \
        ++member_count; \
    } \
}    

#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE(P1, P2, P3, Seq, Count) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_LAST(P1, P2, P3, Seq, Count) if ((num_params-Count) < num_mandatory_params) MCP_DETAIL_JSONCONS_PP_EXPAND(MCP_DETAIL_JSONCONS_PP_CONCAT(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_,MCP_DETAIL_JSONCONS_NARGS Seq) Seq)
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_3(Getter, Setter, Name) \
{ \
    auto r = try_encode_member(string_view_type(Name), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(string_view_type(Name), val.Getter(), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
}
 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_5(Getter, Setter, Name, Mode, Match) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_7(Getter, Setter, Name, Mode, Match, , )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_6(Getter, Setter, Name, Mode, Match, Into) MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_7(Getter, Setter, Name, Mode, Match, Into, )
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE_7(Getter, Setter, Name, Mode, Match, Into, From) \
{ \
    auto r = try_encode_member(string_view_type(Name), Into(val.Getter()), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
} \
else \
{ \
    auto r = try_encode_optional_member(string_view_type(Name), Into(val.Getter()), encoder); \
    if (MCP_DETAIL_JSONCONS_UNLIKELY(!r)) {return r;} \
}

#define MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_TRAITS_BASE(ToJson, NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <typename Json MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_MORE_TPL_PARAM, NumTemplateParams)> \
    struct json_traits<Json, TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        using result_type = conversion_result<value_type>; \
        using char_type = typename Json::char_type; \
        using string_view_type = typename Json::string_view_type; \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        static bool is(const Json& ajson) noexcept \
        { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_IS,,,, __VA_ARGS__)\
            return true; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) \
        { \
            const char* class_name = # TypeName; \
            std::error_code ec; \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map, class_name); \
            value_type class_instance = mcp_detail_jsoncons::make_obj_using_allocator<value_type>(aset.get_allocator()); \
            if (num_params == num_mandatory_params) \
            { \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_AS,,,, __VA_ARGS__) \
            } \
            else \
            { \
                std::size_t index = 0; \
                MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_AS,,,, __VA_ARGS__) \
            } \
            return result_type(std::move(class_instance)); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& class_instance) \
        { \
            Json ajson = mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), json_object_arg, semantic_tag::none); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(ToJson,,,, __VA_ARGS__) \
            return ajson; \
        } \
    }; \
    template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> \
    struct encode_traits<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> \
    { \
        using value_type = TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams); \
        constexpr static size_t num_params = MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__); \
        constexpr static size_t num_mandatory_params = NumMandatoryParams; \
        template <typename Alloc,typename TempAlloc,typename CharT> \
        static write_result encode(const allocator_set<Alloc,TempAlloc>&, const value_type& val, \
            basic_json_visitor<CharT>& encoder) \
        { \
            using char_type = CharT; \
            using string_view_type = basic_string_view<char_type>; \
            (void)num_params; (void)num_mandatory_params; (void)num_mandatory_params; \
            std::error_code ec; \
            std::size_t member_count{0}; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_COUNT,,,, __VA_ARGS__) \
            encoder.begin_object(member_count, semantic_tag::none, ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_ENCODE,,,, __VA_ARGS__) \
            encoder.end_object(ser_context(), ec); \
            if (MCP_DETAIL_JSONCONS_UNLIKELY(ec)) {return write_result{unexpect, ec};} \
            return write_result{}; \
        } \
    }; \
} \
} \
  /**/
 
#define MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TRAITS(TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON, 0, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_TPL_N_GETTER_SETTER_NAME_TRAITS(NumTemplateParams, TypeName,NumMandatoryParams, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_N_GETTER_SETTER_NAME_TO_JSON, NumTemplateParams, TypeName,NumMandatoryParams, __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TRAITS(TypeName, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON, 0, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <> struct is_json_type_traits_declared<TypeName> : public std::true_type {}; } \
  /**/
 
#define MCP_DETAIL_JSONCONS_TPL_ALL_GETTER_SETTER_NAME_TRAITS(NumTemplateParams, TypeName, ...)  \
    MCP_DETAIL_JSONCONS_GETTER_SETTER_NAME_TRAITS_BASE(MCP_DETAIL_JSONCONS_ALL_GETTER_SETTER_NAME_TO_JSON, NumTemplateParams, TypeName, MCP_DETAIL_JSONCONS_NARGS(__VA_ARGS__),  __VA_ARGS__) \
    namespace mcp_detail_jsoncons { template <MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAMS(MCP_DETAIL_JSONCONS_GENERATE_TPL_PARAM, NumTemplateParams)> struct is_json_type_traits_declared<TypeName MCP_DETAIL_JSONCONS_GENERATE_TPL_ARGS(MCP_DETAIL_JSONCONS_GENERATE_TPL_ARG, NumTemplateParams)> : public std::true_type {}; } \
  /**/

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_IS(BaseClass, P2, P3, DerivedClass, Count) if (ajson.template is<DerivedClass>()) return true;
#define MCP_DETAIL_JSONCONS_POLYMORPHIC_IS_LAST(BaseClass, P2, P3, DerivedClass, Count)  if (ajson.template is<DerivedClass>()) return true;

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_UNIQUE_PTR(BaseClass, P2, P3, DerivedClass, Count) { \
  auto result = ajson.template try_as<DerivedClass>(aset); \
  if (result) { \
  using rebind = typename std::allocator_traits<Alloc>::template rebind_alloc<DerivedClass>; \
  auto alloc = rebind(aset.get_allocator()); \
  auto* ptr = alloc.allocate(1); \
  MCP_DETAIL_JSONCONS_TRY {ptr = new(ptr) DerivedClass(*result);} MCP_DETAIL_JSONCONS_CATCH(...) {alloc.deallocate(ptr,1); throw;} \
  return result_type{mcp_detail_jsoncons::in_place, ptr, mcp_detail_jsoncons::make_obj_using_allocator<Deleter>(alloc)};} \
} /**/

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_UNIQUE_PTR_LAST(BaseClass, P2, P3, DerivedClass, Count) { \
  auto result = ajson.template try_as<DerivedClass>(aset); \
  if (result) { \
  using rebind = typename std::allocator_traits<Alloc>::template rebind_alloc<DerivedClass>; \
  auto alloc = rebind(aset.get_allocator()); \
  auto* ptr = alloc.allocate(1); \
  MCP_DETAIL_JSONCONS_TRY {ptr = new(ptr) DerivedClass(*result);} MCP_DETAIL_JSONCONS_CATCH(...) {alloc.deallocate(ptr,1); throw;} \
  return result_type{mcp_detail_jsoncons::in_place, ptr, mcp_detail_jsoncons::make_obj_using_allocator<Deleter>(alloc)};} \
} /**/

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_SHARED_PTR(BaseClass, P2, P3, DerivedClass, Count) { \
  auto result = ajson.template try_as<DerivedClass>(aset); \
  if (result) {return result_type(std::allocate_shared<DerivedClass>(aset.get_allocator(), std::move(*result)));} \
} /**/

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_SHARED_PTR_LAST(BaseClass, P2, P3, DerivedClass, Count) { \
  auto result = ajson.template try_as<DerivedClass>(aset); \
  if (result) {return result_type(std::allocate_shared<DerivedClass>(aset.get_allocator(), std::move(*result)));} \
} /**/
 
#define MCP_DETAIL_JSONCONS_POLYMORPHIC_TO_JSON(BaseClass, P2, P3, DerivedClass, Count) if (DerivedClass* p = dynamic_cast<DerivedClass*>(ptr.get())) {return mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), *p);}
#define MCP_DETAIL_JSONCONS_POLYMORPHIC_TO_JSON_LAST(BaseClass, P2, P3, DerivedClass, Count) if (DerivedClass* p = dynamic_cast<DerivedClass*>(ptr.get())) {return mcp_detail_jsoncons::make_obj_using_allocator<Json>(aset.get_allocator(), *p);}

#define MCP_DETAIL_JSONCONS_POLYMORPHIC_TRAITS(BaseClass, ...)  \
namespace mcp_detail_jsoncons { \
namespace reflect { \
    template <typename Json> \
    struct json_traits<Json, std::shared_ptr<BaseClass>> { \
        using value_type = std::shared_ptr<BaseClass>; \
        using result_type = conversion_result<value_type>; \
        static bool is(const Json& ajson) noexcept { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_IS, BaseClass,,, __VA_ARGS__)\
            return false; \
        } \
\
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) { \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_SHARED_PTR, BaseClass,,, __VA_ARGS__)\
            return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed); \
        } \
\
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& ptr) { \
            if (ptr.get() == nullptr) {return Json::null();} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_TO_JSON, BaseClass,,, __VA_ARGS__)\
            return Json::null(); \
        } \
    }; \
    template <typename Json,typename Deleter> \
    struct json_traits<Json, std::unique_ptr<BaseClass,Deleter>> { \
        using value_type = std::unique_ptr<BaseClass,Deleter>; \
        using result_type = conversion_result<value_type>; \
        static bool is(const Json& ajson) noexcept { \
            if (!ajson.is_object()) return false; \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_IS, BaseClass,,, __VA_ARGS__)\
            return false; \
        } \
        template <typename Alloc,typename TempAlloc> \
        static result_type try_as(const allocator_set<Alloc,TempAlloc>& aset, const Json& ajson) { \
            if (!ajson.is_object()) return result_type(mcp_detail_jsoncons::unexpect, conv_errc::not_map); \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_AS_UNIQUE_PTR, BaseClass,,, __VA_ARGS__)\
            return result_type(mcp_detail_jsoncons::unexpect, conv_errc::conversion_failed); \
        } \
        template <typename Alloc,typename TempAlloc> \
        static Json to_json(const allocator_set<Alloc,TempAlloc>& aset, const value_type& ptr) { \
            if (ptr.get() == nullptr) {return Json::null();} \
            MCP_DETAIL_JSONCONS_VARIADIC_FOR_EACH(MCP_DETAIL_JSONCONS_POLYMORPHIC_TO_JSON, BaseClass,,, __VA_ARGS__)\
            return Json::null(); \
        } \
    }; \
} \
} \
  /**/

#endif // MCP_DETAIL_JSONCONS_REFLECT_REFLECT_TRAITS_GEN_HPP
