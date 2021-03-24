#pragma once

#define LITERPC_ENABLE_ZSTD

#include <typeinfo>
#include <string>
#include <mutex>
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
	constexpr auto MAX_BODY_LENGTH = 8_m;

	enum class magic_num :uint8_t { //for check valid head
		first = 57,
		second = 191
	};

	enum class msg_type : uint8_t {
		req_res,
		sub_pub,
		cancel_sub_pub,
		keepalived
	};

#pragma pack (1)
	struct header {
		uint64_t msg_id; //increase 1 by every request, use for client request callback.
		uint32_t body_length;
		magic_num first = magic_num::first;
		uint8_t name_length; //for remote method name or subscribe topic name.
		msg_type type;
		magic_num second = magic_num::second;
		uint16_t tag_length; //for subscribe tag
	};
#pragma pack ()

	inline void make_head(header& h, uint64_t msg_id, uint32_t body_len, uint8_t name_length, msg_type type, uint16_t tag_length = 0)
	{
		h.msg_id = msg_id;
		h.body_length = body_len;
		h.name_length = name_length;
		h.type = type;
		h.tag_length = tag_length;
	}

	template<typename T>
	inline msgpack::sbuffer serialize(T&& t) {
		msgpack::sbuffer sb;
		msgpack::pack(sb, std::forward<T>(t));
		return sb;
	}

	template<typename T>
	inline T deserialize(const char* buf, std::size_t len) {
		try {
			auto obj_handle = msgpack::unpack(buf, len);
			return obj_handle.get().as<T>();
		}
		catch (...) {
			throw std::invalid_argument(std::string("deserialize failed: Type not match, type is ") + typeid(T{}).name());
		}
	}

	inline auto split_tag_hash(std::string_view tags) {
		std::vector<size_t> tags_hash;
		auto pos = tags.find("||");
		while (pos != std::string_view::npos) {
			auto tag = tags.substr(0, pos);
			tags_hash.emplace_back(std::hash<std::string_view>()(tag));

			tags = tags.substr(pos + 2);
			pos = tags.find("||");
		}
		tags_hash.emplace_back(std::hash<std::string_view>()(tags));
		return tags_hash;
	}

	inline auto split_tag_string(std::string_view tags) {
		std::vector<std::string> tags_str;
		auto pos = tags.find("||");
		while (pos != std::string::npos) {
			auto tag = tags.substr(0, pos);
			tags_str.emplace_back(tag);

			tags = tags.substr(pos + 2);
			pos = tags.find("||");
		}
		tags_str.emplace_back(tags);
		return tags_str;
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

	template<typename SrcType>
	inline size_t compress(compress_detail& detail, SrcType&& src, std::string& dst)
	{
		size_t src_size = src.size();
		if (src_size < COMPRESS_THRESHOLD) {
			return src_size;
		}

		//need compress
		dst.resize(src_size);
		auto compress_length = ZSTD_compressCCtx(detail.cctx, dst.data(), dst.length(), src.data(), src_size, 10);
		return compress_length;
	}

	inline std::string_view decompress(compress_detail& detail, std::string_view src) {
		if (src.length() <= 4) { //can not be compressed
			return src;
		}

		uint32_t magic_num = *((uint32_t*)src.data());
		if (magic_num != ZSTD_MAGICNUMBER) { //can not be compressed	 			
			return src;
		}
		//extremely compressed data, maybe not
		auto decompress_length = ZSTD_getDecompressedSize(src.data(), src.length());
		if (decompress_length == 0) {//get decompress_length error, maybe not compressed data
			return src;
		}

		//need decompress
		if (decompress_length > detail.buf.size()) {
			detail.buf.resize((size_t)decompress_length);
		}
		auto origin_body_length = ZSTD_decompressDCtx(detail.dctx, detail.buf.data(), detail.buf.size(), src.data(), src.length());
		assert(origin_body_length == decompress_length);
		return{ detail.buf.data() ,origin_body_length };
	}
#endif
}



