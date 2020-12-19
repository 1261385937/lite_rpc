#pragma once
#ifndef IOCONTEXTPOOL_H
#define IOCONTEXTPOOL_H

#include <memory>
#include <vector>
#include <thread>
#include "boost/asio/io_context.hpp"

namespace lite_rpc {
	class io_context_pool
	{
	private:
		size_t io_context_index_;
		std::vector<std::unique_ptr<std::thread>> thread_vec_;
		std::vector<std::unique_ptr<boost::asio::io_context>> io_context_vec_;
		std::vector<std::unique_ptr<boost::asio::io_context::work>> work_vec_; //keep io_context.run() alive
	public:
		explicit io_context_pool(uint32_t pool_size) 
			:io_context_index_(0) 
		{
			for (uint32_t i = 0; i < pool_size; ++i) {
				auto io_context_ptr = std::make_unique<boost::asio::io_context>();
				auto work_ptr = std::make_unique<boost::asio::io_context::work>(*io_context_ptr);
				io_context_vec_.emplace_back(std::move(io_context_ptr));
				work_vec_.emplace_back(std::move(work_ptr));
			}
		}

		~io_context_pool() {
			stop();
		}

		void start() {
			auto size = io_context_vec_.size();
			for (size_t i = 0; i < size; ++i) {
				auto thread_ptr = std::make_unique<std::thread>(
					[this, i]() {
					try {
						io_context_vec_[i]->run();
					}
					catch (std::exception& e) {
						printf("io_service thread exit with %s\n", e.what());
					}
					catch (...) {
						printf("io_service thread exit with unknown exception\n");
					}
				});
				thread_vec_.emplace_back(std::move(thread_ptr));
			}
		}

		boost::asio::io_context& get_io_context() {
			auto& io_context = *(io_context_vec_[io_context_index_]);
			++io_context_index_;
			if (io_context_index_ == static_cast<int>(io_context_vec_.size()))
				io_context_index_ = 0;
			return io_context;
		}

	private:
		void stop() {
			auto work_vec_size = work_vec_.size();
			for (size_t i = 0; i < work_vec_size; ++i) {
				//work_vec_[i]->~work();
				work_vec_[i].reset();
				io_context_vec_[i]->stop();
			}

			auto size = thread_vec_.size();
			for (size_t i = 0; i < size; ++i) {
				if (thread_vec_[i]->joinable())
					thread_vec_[i]->join();
			}
		}

		io_context_pool(const io_context_pool&) = delete;
		io_context_pool& operator=(const io_context_pool&) = delete;
	};
}
#endif