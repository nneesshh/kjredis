// The MIT License (MIT)
//
// Copyright (c) 2015-2017 Simon Ninon <simon.ninon@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <string>

#include "builders/builder_iface.hpp"

namespace cpp_redis {

namespace builders {

class simple_string_builder : public builder_iface {
public:
	//! ctor & dtor
	simple_string_builder();
	~simple_string_builder() = default;

	//! copy ctor & assignment operator
	simple_string_builder(const simple_string_builder&) = delete;
	simple_string_builder& operator=(const simple_string_builder&) = delete;

public:
	//! builder_iface impl
	builder_iface& operator<<(bip_buf_t& bbuf);
	bool reply_ready() const;
	CRedisReply&& get_reply();

private:
	std::string _str;

	bool _reply_ready = false;
	CRedisReply _reply;
};

} //! builders

} //! cpp_redis
