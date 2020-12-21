#pragma once
#include <type_traits>
#include <tuple>
#include <functional>

namespace lite_rpc {
	template<typename ResType, typename... Args>
	struct function_traits_helper {
		using return_type = ResType;
		static constexpr auto args_size_v = sizeof...(Args);
		using tuple_args_t = std::tuple<Args...>;
		using nonref_tuple_args_t = std::tuple<std::remove_cv_t<std::remove_reference_t<Args>>...>;
	};

	template<typename F>
	struct function_traits;

	template<typename ResType, typename...Args>
	struct function_traits<std::function<ResType(Args...)>> : function_traits_helper<ResType, Args...> {};

	template<typename ResType, typename...Args>
	struct function_traits<ResType(*)(Args...)> : function_traits_helper<ResType, Args...> {};

	template<typename ResType, typename...Args>
	struct function_traits<ResType(Args...)> : function_traits_helper<ResType, Args...> {};

	template <typename ClassType, typename ResType, typename... Args>
	struct function_traits<ResType(ClassType::*)(Args...)> : function_traits_helper<ResType, Args...> {
		using class_type = ClassType;
	};

	template <typename ClassType, typename ResType, typename... Args>
	struct function_traits<ResType(ClassType::*)(Args...) const> : function_traits_helper<ResType, Args...> {
		using class_type = ClassType;
	};

	template <typename LambdaType>
	struct function_traits :function_traits<decltype(&LambdaType::operator())> {};

	template <typename>
	inline constexpr bool always_false_v = false;

	template<typename T>
	inline constexpr bool is_char_array_v =
		std::is_array_v<T> && std::is_same_v<char, std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<std::decay_t<T>>>>>;

}