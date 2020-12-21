#pragma once

#define LITERPC_ENABLE_ZSTD

#include "msgpack.hpp"

constexpr decltype(auto) operator""_k(unsigned long long n) {
	return n * 1024;
}

constexpr decltype(auto) operator""_m(unsigned long long n) {
	return n * 1024_k;
}


#ifdef LITERPC_ENABLE_ZSTD
#include "zstd.h"
constexpr auto COMPRESS_THRESHOLD = 1_k;//default 1K
#endif


namespace lite_rpc {

	constexpr auto MAX_BODY_SIZE = 128_k;

	enum class protocol_major :uint8_t { //for protocol version
		first
	};

	enum class protocol_minor :uint8_t { //for protocol version
		zero
	};

	enum class request_type : uint8_t {
		req_res,
		sub_pub,
		keepalived
	};

	struct header { //note byte align
		uint64_t msg_id; //increase 1 by every request, use for client request callback.
		protocol_major major;
		protocol_minor minor;
		uint16_t name_length; //for remote method name or subscribe key name
		uint32_t decompress_length; //if decompress_length equal body_length means the body is not compressed.
		uint32_t body_length;
		request_type req_type;
		uint8_t reserve1 = 0;  //unused, now for byte align
		uint16_t reserve2 = 0; //unused, now for byte align
	};

	inline void make_head_v1_0(header& h, uint32_t decompress_length, uint32_t body_len, uint64_t id, request_type req_type, uint16_t name_length)
	{
		h.msg_id = id;
		h.major = protocol_major::first;
		h.minor = protocol_minor::zero;
		h.decompress_length = decompress_length;
		h.body_length = body_len;
		h.req_type = req_type;
		h.name_length = name_length;
	}

	template<typename T>
	msgpack::sbuffer serialize(T&& t) {
		msgpack::sbuffer sb;
		msgpack::pack(sb, std::forward<T>(t));
		return sb;
	}

	template<typename T>
	T deserialize(const char* buf, std::size_t len) {
		try {
			auto obj_handle = msgpack::unpack(buf, len);
			return obj_handle.get().as<T>();
		}
		catch (...) {
			throw std::invalid_argument("deserialize failed: Type not match");
		}
	}

	struct compress_detail {
#ifdef LITERPC_ENABLE_ZSTD
		std::vector<char> buf;
		ZSTD_CCtx* cctx;
		ZSTD_DCtx* dctx;
		std::mutex compress_mtx;

		compress_detail() {
			buf.resize((size_t)8_k);
			cctx = ZSTD_createCCtx();
			dctx = ZSTD_createDCtx();
		}

		~compress_detail() {
			ZSTD_freeCCtx(cctx);
			ZSTD_freeDCtx(dctx);
		}
#endif
	};
	

#ifdef LITERPC_ENABLE_ZSTD
	template<typename SrcType>
	inline size_t compress_with_mutex(ZSTD_CCtx* cctx, std::mutex& mtx, SrcType&& src, std::string& dst)
	{
		size_t src_size = src.size();
		if (src_size < COMPRESS_THRESHOLD) {
			return src_size;
		}

		//need compress
		dst.resize(src_size);
		std::unique_lock<std::mutex> l(mtx);
		auto compress_length = ZSTD_compressCCtx(cctx, dst.data(), dst.length(), src.data(), src_size, 10);
		l.unlock();
		return compress_length;
	}

	inline std::string_view compress(compress_detail& detail, std::string_view src)
	{
		size_t src_size = src.size();
		if (src_size < COMPRESS_THRESHOLD) {
			return src;
		}

		//need compress
		if (src_size > detail.buf.size()) {
			detail.buf.resize(src_size); //the output length can not be bigger than input length.
		}
		auto length = ZSTD_compressCCtx(detail.cctx, detail.buf.data(), detail.buf.size(), src.data(), src_size, 10);
		return { detail.buf.data() ,length };
	}

	inline std::string_view decompress(compress_detail& detail, std::string_view src, size_t decompress_length)
	{
		auto src_size = src.length();
		if (src_size == decompress_length) {
			return src;
		}

		//need decompress
		if (decompress_length > detail.buf.size()) {
			detail.buf.resize(decompress_length);
		}
		auto origin_body_length = ZSTD_decompressDCtx(detail.dctx, detail.buf.data(), detail.buf.size(), src.data(), src_size);
		assert(origin_body_length == decompress_length);
		return{ detail.buf.data() ,origin_body_length };
	}
#endif
}



