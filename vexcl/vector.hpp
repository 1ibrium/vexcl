#ifndef VEXCL_VECTOR_HPP
#define VEXCL_VECTOR_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vector.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  OpenCL device vector.
 */

#ifdef WIN32
#  pragma warning(push)
#  pragma warning(disable : 4267 4290)
#  define NOMINMAX
#endif

#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <functional>
#include <boost/proto/proto.hpp>
#include <vexcl/util.hpp>
#include <vexcl/profiler.hpp>
#include <vexcl/operations.hpp>

/// Vector expression template library for OpenCL.
namespace vex {

/// \cond INTERNAL

//--- Vector Type -----------------------------------------------------------
/// Device vector.
template <typename T>
struct vector
    : vector_expression< typename boost::proto::terminal< vector_terminal >::type >
{
    public:
	/// Proxy class.
	/**
	 * Instances of this class are returned from vector::operator[]. These
	 * may be used to read or write single element of a vector, although
	 * this operations are too expensive to be used extensively and should
	 * be reserved for debugging purposes.
	 */
	class element {
	    public:
		/// Read associated element of a vector.
		operator T() const {
		    T val;
		    queue.enqueueReadBuffer(
			    buf, CL_TRUE,
			    index * sizeof(T), sizeof(T),
			    &val
			    );
		    return val;
		}

		/// Write associated element of a vector.
		T operator=(T val) {
		    queue.enqueueWriteBuffer(
			    buf, CL_TRUE,
			    index * sizeof(T), sizeof(T),
			    &val
			    );
		    return val;
		}
	    private:
		element(const cl::CommandQueue &q, cl::Buffer b, size_t i)
		    : queue(q), buf(b), index(i) {}

		const cl::CommandQueue  &queue;
		cl::Buffer              buf;
		const size_t            index;

		friend class vector;
	};

	/// Iterator class.
	/**
	 * This class may in principle be used with standard template library,
	 * although its main purpose is range specification for vector copy
	 * operations.
	 */
	template <class vector_type, class element_type>
	class iterator_type
	    : public std::iterator<std::random_access_iterator_tag, T>
	{
	    public:
		static const bool device_iterator = true;

		element_type operator*() const {
		    return element_type(
			    vec.queue[part], vec.buf[part],
			    pos - vec.part[part]
			    );
		}

		iterator_type& operator++() {
		    pos++;
		    while (part < vec.nparts() && pos >= vec.part[part + 1])
			part++;
		    return *this;
		}

		iterator_type operator+(ptrdiff_t d) const {
		    return iterator_type(vec, pos + d);
		}

		ptrdiff_t operator-(iterator_type it) const {
		    return pos - it.pos;
		}

		bool operator==(const iterator_type &it) const {
		    return pos == it.pos;
		}

		bool operator!=(const iterator_type &it) const {
		    return pos != it.pos;
		}

		vector_type &vec;
		size_t  pos;
		size_t  part;

	    private:
		iterator_type(vector_type &vec, size_t pos)
		    : vec(vec), pos(pos), part(0)
		{
		    if (!vec.part.empty()) {
			part = std::upper_bound(
				vec.part.begin(), vec.part.end(), pos
				) - vec.part.begin() - 1;
		    }
		}

		friend class vector;
	};

	typedef iterator_type<vector, element> iterator;
	typedef iterator_type<const vector, const element> const_iterator;

	/// Empty constructor.
	vector() {}

	/// Copy constructor.
	vector(const vector &v)
	    : queue(v.queue), part(v.part),
	      buf(queue.size()), event(queue.size())
	{
	    if (size()) allocate_buffers(CL_MEM_READ_WRITE, 0);
	    *this = v;
	}

	/// Copy host data to the new buffer.
	vector(const std::vector<cl::CommandQueue> &queue,
		size_t size, const T *host = 0,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      ) : queue(queue), part(vex::partition(size, queue)),
	          buf(queue.size()), event(queue.size())
	{
	    if (size) allocate_buffers(flags, host);
	}

	/// Copy host data to the new buffer.
	vector(const std::vector<cl::CommandQueue> &queue,
		const std::vector<T> &host,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      ) : queue(queue), part(vex::partition(host.size(), queue)),
		  buf(queue.size()), event(queue.size())
	{
	    if (!host.empty()) allocate_buffers(flags, host.data());
	}

	/// Move constructor
	vector(vector &&v) {
	    swap(v);
	}

	/// Move assignment
	const vector& operator=(vector &&v) {
	    swap(v);
	    return *this;
	}

	/// Swap function.
	void swap(vector &v) {
	    std::swap(queue,   v.queue);
	    std::swap(part,    v.part);
	    std::swap(buf,     v.buf);
	    std::swap(event,   v.event);
	}

	/// Resize vector.
	void resize(const vector &v, cl_mem_flags flags = CL_MEM_READ_WRITE)
	{
	    // Reallocate bufers
	    *this = std::move(vector(v.queue, v.size(), 0, flags));

	    // Copy data
	    *this = v;
	}

	/// Resize vector.
	void resize(const std::vector<cl::CommandQueue> &queue,
		size_t size, const T *host = 0,
		cl_mem_flags flags = CL_MEM_READ_WRITE
		)
	{
	    *this = std::move(vector(queue, size, host, flags));
	}

	/// Resize vector.
	void resize(const std::vector<cl::CommandQueue> &queue,
		const std::vector<T> &host,
		cl_mem_flags flags = CL_MEM_READ_WRITE
	      )
	{
	    *this = std::move(vector(queue, host, flags));
	}

	/// Return cl::Buffer object located on a given device.
	cl::Buffer operator()(uint d = 0) const {
	    return buf[d];
	}

	/// Const iterator to beginning.
	const_iterator begin() const {
	    return const_iterator(*this, 0);
	}

	/// Const iterator to end.
	const_iterator end() const {
	    return const_iterator(*this, size());
	}

	/// Iterator to beginning.
	iterator begin() {
	    return iterator(*this, 0);
	}

	/// Iterator to end.
	iterator end() {
	    return iterator(*this, size());
	}

	/// Access element.
	const element operator[](size_t index) const {
	    uint d = std::upper_bound(
		    part.begin(), part.end(), index) - part.begin() - 1;
	    return element(queue[d], buf[d], index - part[d]);
	}

	/// Access element.
	element operator[](size_t index) {
	    uint d = static_cast<uint>(
		std::upper_bound(part.begin(), part.end(), index) - part.begin() - 1
		);
	    return element(queue[d], buf[d], index - part[d]);
	}

	/// Return size .
	size_t size() const {
	    return part.empty() ? 0 : part.back();
	}

	/// Return number of parts (devices).
	uint nparts() const {
	    return queue.size();
	}

	/// Return size of part on a given device.
	size_t part_size(uint d) const {
	    return part[d + 1] - part[d];
	}

	/// Return part start for a given device.
	size_t part_start(uint d) const {
	    return part[d];
	}

	/// Return reference to vector's queue list
	const std::vector<cl::CommandQueue>& queue_list() const {
	    return queue;
	}

	/// Return reference to vector's partition.
	const std::vector<size_t>& partition() const {
	    return part;
	}

	/// Copies data from device vector.
	const vector& operator=(const vector &x) {
	    if (&x != this) {
		for(uint d = 0; d < queue.size(); d++)
		    if (size_t psize = part[d + 1] - part[d]) {
			queue[d].enqueueCopyBuffer(x.buf[d], buf[d], 0, 0,
				psize * sizeof(T));
		    }
	    }

	    return *this;
	}

	/** \name Expression assignments.
	 * @{
	 * The appropriate kernel is compiled first time the assignment is
	 * made. Vectors participating in expression should have same number of
	 * parts; corresponding parts of the vectors should reside on the same
	 * compute devices.
	 */
	template <class Expr>
	typename std::enable_if<
	    boost::proto::matches<
		typename boost::proto::result_of::as_expr<Expr>::type,
		vector_expr_grammar
	    >::value,
	    const vector&
	>::type
	operator=(const Expr &expr) {
	    for(auto q = queue.begin(); q != queue.end(); q++) {
		cl::Context context = qctx(*q);
		cl::Device  device  = qdev(*q);

		if (!exdata<Expr>::compiled[context()]) {
		    std::ostringstream kernel;

		    vector_expr_context expr_ctx(kernel);

		    std::ostringstream kernel_name;
		    vector_name_context name_ctx(kernel_name);
		    boost::proto::eval(boost::proto::as_child(expr), name_ctx);

		    kernel << standard_kernel_header;

		    extract_user_functions()(
			    boost::proto::as_child(expr),
			    declare_user_function(kernel)
			    );

		    kernel << "kernel void " << kernel_name.str()
		           << "(\n\t" << type_name<size_t>()
			   << " n,\n\tglobal " << type_name<T>() << " *res";

		    extract_terminals()(
			    boost::proto::as_child(expr),
			    declare_expression_parameter(kernel)
			    );

		    kernel <<
			"\n)\n{\n\t"
			"for(size_t idx = get_global_id(0); idx < n; idx += get_global_size(0)) {\n"
			"\t\tres[idx] = ";

		    boost::proto::eval(boost::proto::as_child(expr), expr_ctx);

		    kernel << ";\n\t}\n}\n";

#ifdef VEXCL_SHOW_KERNELS
		    std::cout << kernel.str() << std::endl;
#endif

		    auto program = build_sources(context, kernel.str());

		    exdata<Expr>::kernel[context()]   = cl::Kernel(program, kernel_name.str().c_str());
		    exdata<Expr>::compiled[context()] = true;
		    exdata<Expr>::wgsize[context()]   = kernel_workgroup_size(
			    exdata<Expr>::kernel[context()], device);
		}
	    }

	    for(uint d = 0; d < queue.size(); d++) {
		if (size_t psize = part[d + 1] - part[d]) {
		    cl::Context context = qctx(queue[d]);
		    cl::Device  device  = qdev(queue[d]);

		    size_t g_size = device.getInfo<CL_DEVICE_TYPE>() == CL_DEVICE_TYPE_CPU ?
			alignup(psize, exdata<Expr>::wgsize[context()]) :
			device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() * exdata<Expr>::wgsize[context()] * 4;

		    uint pos = 0;
		    exdata<Expr>::kernel[context()].setArg(pos++, psize);
		    exdata<Expr>::kernel[context()].setArg(pos++, buf[d]);

		    extract_terminals()(
			    boost::proto::as_child(expr),
			    set_expression_argument(exdata<Expr>::kernel[context()], d, pos)
			    );

		    queue[d].enqueueNDRangeKernel(
			    exdata<Expr>::kernel[context()],
			    cl::NullRange,
			    g_size, exdata<Expr>::wgsize[context()]
			    );
		}
	    }

	    return *this;
	}

	template <class Expr>
	const vector& operator=(const spmv_expression<Expr> &expr) {
	    assign_spmv_context assign(*this);
	    boost::proto::eval(expr, assign);

	    return *this;
	}

#define COMPOUND_ASSIGNMENT(cop, op) \
	template <class Expr> \
	const vector& operator cop(const Expr &expr) { \
	    return *this = *this op expr; \
	}

	COMPOUND_ASSIGNMENT(+=, +);
	COMPOUND_ASSIGNMENT(-=, -);
	COMPOUND_ASSIGNMENT(*=, *);
	COMPOUND_ASSIGNMENT(/=, /);
	COMPOUND_ASSIGNMENT(%=, %);
	COMPOUND_ASSIGNMENT(&=, &);
	COMPOUND_ASSIGNMENT(|=, |);
	COMPOUND_ASSIGNMENT(^=, ^);
	COMPOUND_ASSIGNMENT(<<=, <<);
	COMPOUND_ASSIGNMENT(>>=, >>);

#undef COMPOUND_ASSIGNMENT

	/// Copy data from host buffer to device(s).
	void write_data(size_t offset, size_t size, const T *hostptr, cl_bool blocking,
		std::vector<cl::Event> *uevent = 0)
	{
	    if (!size) return;

	    std::vector<cl::Event> &ev = uevent ? *uevent : event;

	    for(uint d = 0; d < queue.size(); d++) {
		size_t start = std::max(offset,        part[d]);
		size_t stop  = std::min(offset + size, part[d + 1]);

		if (stop <= start) continue;

		queue[d].enqueueWriteBuffer(buf[d], CL_FALSE,
			sizeof(T) * (start - part[d]),
			sizeof(T) * (stop - start),
			hostptr + start - offset,
			0, &ev[d]
			);
	    }

	    if (blocking)
		for(size_t d = 0; d < queue.size(); d++) {
		    size_t start = std::max(offset,        part[d]);
		    size_t stop  = std::min(offset + size, part[d + 1]);

		    if (start < stop) ev[d].wait();
		}
	}

	/// Copy data from device(s) to host buffer .
	void read_data(size_t offset, size_t size, T *hostptr, cl_bool blocking,
		std::vector<cl::Event> *uevent = 0) const
	{
	    if (!size) return;

	    std::vector<cl::Event> &ev = uevent ? *uevent : event;

	    for(uint d = 0; d < queue.size(); d++) {
		size_t start = std::max(offset,        part[d]);
		size_t stop  = std::min(offset + size, part[d + 1]);

		if (stop <= start) continue;

		queue[d].enqueueReadBuffer(buf[d], CL_FALSE,
			sizeof(T) * (start - part[d]),
			sizeof(T) * (stop - start),
			hostptr + start - offset,
			0, &ev[d]
			);
	    }

	    if (blocking)
		for(uint d = 0; d < queue.size(); d++) {
		    size_t start = std::max(offset,        part[d]);
		    size_t stop  = std::min(offset + size, part[d + 1]);

		    if (start < stop) ev[d].wait();
		}
	}

	/// \endcond

    private:
	template <class Expr>
	struct exdata {
	    static std::map<cl_context,bool>       compiled;
	    static std::map<cl_context,cl::Kernel> kernel;
	    static std::map<cl_context,size_t>     wgsize;
	};

	std::vector<cl::CommandQueue>	queue;
	std::vector<size_t>             part;
	std::vector<cl::Buffer>		buf;
	mutable std::vector<cl::Event>  event;

	void allocate_buffers(cl_mem_flags flags, const T *hostptr) {
	    for(uint d = 0; d < queue.size(); d++) {
		if (size_t psize = part[d + 1] - part[d]) {
		    cl::Context context = qctx(queue[d]);

		    buf[d] = cl::Buffer(context, flags, psize * sizeof(T));
		}
	    }
	    if (hostptr) write_data(0, size(), hostptr, CL_TRUE);
	}

	struct assign_spmv_context {
	    vector &v;

	    assign_spmv_context(vector &v) : v(v) {}

	    template <typename Expr, typename Tag = typename Expr::proto_tag>
	    struct eval {};

	    template <typename Expr>
	    struct eval<Expr, boost::proto::tag::plus> {
		typedef void result_type;

		template <typename Term>
		void operator()(const Term &term, assign_spmv_context &ctx) const {
		    std::cout << "left: " << sizeof(term) << " ";
		    boost::proto::display_expr(boost::proto::left(term));
		    std::cout << "right: " << sizeof(term) << " ";
		    boost::proto::display_expr(boost::proto::right(term));
		    ctx.v = boost::proto::left(term);
		    boost::proto::right(term).mul(ctx.v);
		}
	    };

	    template <typename Expr>
	    struct eval<Expr, boost::proto::tag::terminal> {
		typedef void result_type;

		template <class Term>
		void operator()(const Term &term, assign_spmv_context &ctx) const {
		    std::cout << "spmv: " << sizeof(term) << " ";
		    boost::proto::display_expr(term);
		    //spmv.mul(ctx.v);
		}
	    };
	};
};

template <class T> template <class Expr>
std::map<cl_context, bool> vector<T>::exdata<Expr>::compiled;

template <class T> template <class Expr>
std::map<cl_context, cl::Kernel> vector<T>::exdata<Expr>::kernel;

template <class T> template <class Expr>
std::map<cl_context, size_t> vector<T>::exdata<Expr>::wgsize;

/// Copy device vector to host vector.
template <class T>
void copy(const vex::vector<T> &dv, std::vector<T> &hv, cl_bool blocking = CL_TRUE) {
    dv.read_data(0, dv.size(), hv.data(), blocking);
}

/// Copy host vector to device vector.
template <class T>
void copy(const std::vector<T> &hv, vex::vector<T> &dv, cl_bool blocking = CL_TRUE) {
    dv.write_data(0, dv.size(), hv.data(), blocking);
}

/// \cond INTERNAL

template<class Iterator, class Enable = void>
struct stored_on_device : std::false_type {};

template<class Iterator>
struct stored_on_device<Iterator,
    typename std::enable_if<Iterator::device_iterator>::type
    > : std::true_type {};

/// \endcond

/// Copy range from device vector to host vector.
template<class InputIterator, class OutputIterator>
typename std::enable_if<
    std::is_same<
	typename std::iterator_traits<InputIterator>::value_type,
	typename std::iterator_traits<OutputIterator>::value_type
	>::value &&
    stored_on_device<InputIterator>::value &&
    !stored_on_device<OutputIterator>::value,
    OutputIterator
    >::type
copy(InputIterator first, InputIterator last,
	OutputIterator result, cl_bool blocking = CL_TRUE)
{
    first.vec.read_data(first.pos, last - first, &result[0], blocking);
    return result + (last - first);
}

/// Copy range from host vector to device vector.
template<class InputIterator, class OutputIterator>
typename std::enable_if<
    std::is_same<
	typename std::iterator_traits<InputIterator>::value_type,
	typename std::iterator_traits<OutputIterator>::value_type
	>::value &&
    !stored_on_device<InputIterator>::value &&
    stored_on_device<OutputIterator>::value,
    OutputIterator
    >::type
copy(InputIterator first, InputIterator last,
	OutputIterator result, cl_bool blocking = CL_TRUE)
{
    result.vec.write_data(result.pos, last - first, &first[0], blocking);
    return result + (last - first);
}

/// Swap two vectors.
template <typename T>
void swap(vector<T> &x, vector<T> &y) {
    x.swap(y);
}

/// Returns device weight after simple bandwidth test
inline double device_vector_perf(
	const cl::Context &context, const cl::Device &device
	)
{
    static const size_t test_size = 1024U * 1024U;
    std::vector<cl::CommandQueue> queue(1,
	    cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE)
	    );

    // Allocate test vectors on current device and measure execution
    // time of a simple kernel.
    vex::vector<float> a(queue, test_size);
    vex::vector<float> b(queue, test_size);
    vex::vector<float> c(queue, test_size);

    // Skip the first run.
    a = b + c;

    // Measure the second run.
    profiler prof(queue);
    prof.tic_cl("");
    a = b + c;
    return 1.0 / prof.toc("");
}

/// \endcond

} // namespace vex

#ifdef WIN32
#  pragma warning(pop)
#endif
#endif
