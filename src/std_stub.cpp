// Minimal declarations of commonly used standard-library names. Prepended to
// every file before a single-file parse so that code written against
// <iostream>, <vector>, <string>, <map>, <algorithm>, <cstdio>, <cmath> ...
// type-checks well enough to produce a full AST, without parsing real system
// headers (which costs ~100x more per file).
//
// Members are deliberately permissive (variadic templates, a universal
// `__any` type) - only the *shape* of user code matters, not its semantics.

#include "plag/tokenizer.h"

namespace plag {

const std::string& stdStubPrelude() {
  static const std::string kPrelude = R"STUB(
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef long long int64_t; typedef unsigned long long uint64_t;
typedef int int32_t; typedef unsigned uint32_t;
namespace std {
  typedef unsigned long size_t;
  struct __any {
    __any(); template<class T> __any(const T&);
    template<class T> __any& operator=(const T&);
    template<class T> operator T() const;
    template<class... A> __any operator()(A&&...) const;
    template<class T> __any operator[](const T&) const;
    __any operator*() const; __any* operator->() const;
    __any& operator++(); __any operator++(int); __any& operator--(); __any operator--(int);
  };
  template<class T> __any operator+(const __any&, const T&);
  template<class T> __any operator-(const __any&, const T&);
  template<class T> bool operator==(const __any&, const T&);
  template<class T> bool operator!=(const __any&, const T&);
  template<class T> bool operator<(const __any&, const T&);
  struct ios_base { static const int sync_with_stdio_dummy = 0; static bool sync_with_stdio(bool = true); };
  struct istream {
    template<class T> istream& operator>>(T&);
    explicit operator bool() const; bool eof() const; istream* tie(void*);
    template<class... A> istream& ignore(A...); int get(); int peek();
  };
  struct ostream {
    template<class T> ostream& operator<<(const T&);
    ostream& operator<<(ostream& (*)(ostream&));
    ostream& flush(); template<class... A> ostream& precision(A...);
    template<class... A> ostream& setf(A...);
  };
  extern istream cin; extern ostream cout, cerr, clog;
  ostream& endl(ostream&); ostream& flush(ostream&);
  ostream& fixed(ostream&);
  __any setprecision(int); __any setw(int);
  template<class S> istream& getline(istream&, S&, char = '\n');
  struct string {
    typedef char* iterator;
    string(); string(const char*); string(size_t, char); template<class I> string(I, I);
    size_t size() const; size_t length() const; bool empty() const;
    char& operator[](size_t); const char& operator[](size_t) const;
    string& operator+=(const string&); string& operator+=(char);
    char* begin(); char* end(); const char* begin() const; const char* end() const;
    const char* c_str() const;
    string substr(size_t, size_t = -1) const; template<class T> size_t find(const T&, size_t = 0) const;
    template<class T> size_t rfind(const T&, size_t = -1) const;
    void push_back(char); void pop_back(); char& back(); char& front(); void clear();
    void resize(size_t, char = 0); void reserve(size_t);
    template<class... A> string& insert(A...); template<class... A> string& erase(A...);
    template<class... A> string& replace(A...); template<class... A> int compare(A...) const;
    static const size_t npos = -1;
  };
  string operator+(const string&, const string&); string operator+(const string&, char);
  string operator+(const char*, const string&);
  bool operator==(const string&, const string&); bool operator!=(const string&, const string&);
  bool operator<(const string&, const string&); bool operator>(const string&, const string&);
  string to_string(long long); int stoi(const string&); long long stoll(const string&);
  double stod(const string&);
  template<class A, class B> struct pair {
    A first; B second; pair(); template<class X, class Y> pair(X&&, Y&&);
  };
  template<class A, class B> pair<A, B> make_pair(A, B);
  template<class A, class B> bool operator<(const pair<A, B>&, const pair<A, B>&);
  template<class A, class B> bool operator==(const pair<A, B>&, const pair<A, B>&);
  template<class... T> struct tuple { template<class... A> tuple(A&&...); };
  template<class... T> tuple<T...> make_tuple(T...);
  template<class... T> tuple<T&...> tie(T&...);
  template<class T, class... R> struct vector {
    typedef T* iterator; typedef const T* const_iterator; typedef T value_type;
    vector(); template<class... A> vector(A&&...); vector(size_t, const T& = T());
    T& operator[](size_t); const T& operator[](size_t) const; T& at(size_t);
    size_t size() const; bool empty() const; void clear();
    void push_back(const T&); template<class... A> void emplace_back(A&&...); void pop_back();
    T& back(); T& front(); T* begin(); T* end(); const T* begin() const; const T* end() const;
    T* rbegin(); T* rend(); T* data();
    void resize(size_t, const T& = T()); void reserve(size_t); template<class... A> void assign(A...);
    template<class... A> T* insert(A...); template<class... A> T* erase(A...);
  };
  template<class T, class... R> bool operator==(const vector<T, R...>&, const vector<T, R...>&);
  template<class T, class... R> bool operator<(const vector<T, R...>&, const vector<T, R...>&);
  template<class T, size_t N> struct array {
    T& operator[](size_t); const T& operator[](size_t) const; size_t size() const;
    T* begin(); T* end(); void fill(const T&);
  };
  template<class K, class V, class... R> struct map {
    typedef pair<const K, V>* iterator;
    V& operator[](const K&); size_t size() const; bool empty() const; size_t count(const K&) const;
    iterator find(const K&); iterator begin(); iterator end(); iterator begin() const; iterator end() const; void clear();
    template<class... A> iterator erase(A...); template<class... A> pair<iterator, bool> insert(A...);
    template<class... A> pair<iterator, bool> emplace(A&&...);
    iterator lower_bound(const K&); iterator upper_bound(const K&);
  };
  template<class K, class V, class... R> struct unordered_map : map<K, V> { void reserve(size_t); };
  template<class K, class V, class... R> struct multimap : map<K, V> {};
  template<class K, class... R> struct set {
    typedef K* iterator;
    size_t size() const; bool empty() const; size_t count(const K&) const;
    iterator find(const K&); iterator begin() const; iterator end() const; iterator rbegin(); iterator rend();
    template<class... A> pair<iterator, bool> insert(A...); template<class... A> size_t erase(A...);
    template<class... A> pair<iterator, bool> emplace(A&&...);
    iterator lower_bound(const K&); iterator upper_bound(const K&); void clear();
  };
  template<class K, class... R> struct unordered_set : set<K> { void reserve(size_t); };
  template<class K, class... R> struct multiset : set<K> {};
  template<class T, class... R> struct deque : vector<T> {
    void push_front(const T&); void pop_front(); template<class... A> void emplace_front(A&&...);
  };
  template<class T, class... R> struct list : deque<T> {};
  template<class T, class... R> struct queue {
    void push(const T&); template<class... A> void emplace(A&&...); void pop();
    T& front(); T& back(); size_t size() const; bool empty() const;
  };
  template<class T, class... R> struct stack {
    void push(const T&); template<class... A> void emplace(A&&...); void pop();
    T& top(); size_t size() const; bool empty() const;
  };
  template<class T, class... R> struct priority_queue {
    priority_queue(); template<class... A> priority_queue(A&&...);
    void push(const T&); template<class... A> void emplace(A&&...); void pop();
    const T& top() const; size_t size() const; bool empty() const;
  };
  template<class T = void> struct greater { bool operator()(const T&, const T&) const; };
  template<class T = void> struct less { bool operator()(const T&, const T&) const; };
  template<class F> struct function { function(); template<class G> function(G); template<class... A> __any operator()(A&&...) const; };
  template<size_t N> struct bitset { bool operator[](size_t) const; size_t count() const; void set(size_t); void reset(); };
  template<class I, class... A> void sort(I, I, A...);
  template<class I, class... A> void stable_sort(I, I, A...);
  template<class I> void reverse(I, I);
  template<class I, class T> I find(I, I, const T&);
  template<class I, class T> I lower_bound(I, I, const T&);
  template<class I, class T> I upper_bound(I, I, const T&);
  template<class I, class T> bool binary_search(I, I, const T&);
  template<class I, class T> T accumulate(I, I, T);
  template<class I, class T> void fill(I, I, const T&);
  template<class I, class T> void iota(I, I, T);
  template<class I> I unique(I, I);
  template<class I> I max_element(I, I);
  template<class I> I min_element(I, I);
  template<class I, class T> long count(I, I, const T&);
  template<class I> bool next_permutation(I, I);
  template<class T> const T& max(const T&, const T&);
  template<class T> const T& min(const T&, const T&);
  template<class T> T max(T, T, T);
  template<class T> void swap(T&, T&);
  template<class T> T abs(T);
  template<class T> T&& move(T&);
  template<class T> T gcd(T, T);
  template<class T> T __lg(T);
  double sqrt(double); double pow(double, double); double log(double); double log2(double);
  double exp(double); double sin(double); double cos(double); double atan2(double, double);
  double floor(double); double ceil(double); double fabs(double); double hypot(double, double);
  template<class T> struct numeric_limits { static T max(); static T min(); static T lowest(); };
  struct mt19937 { mt19937(unsigned = 0); unsigned operator()(); };
  template<class... A> int printf(const char*, A...);
}
using std::size_t;
template<class... A> int printf(const char*, A...);
template<class... A> int scanf(const char*, A...);
template<class... A> int puts(A...);
template<class... A> int getchar(A...);
template<class... A> int putchar(A...);
template<class... A> void* memset(void*, A...);
template<class... A> void* memcpy(void*, A...);
template<class... A> int strlen(A...);
template<class... A> int strcmp(A...);
template<class... A> void exit(A...);
template<class... A> void* malloc(A...);
template<class... A> void free(A...);
template<class... A> int rand(A...);
template<class... A> void srand(A...);
template<class... A> long time(A...);
template<class... A> int abs(A...);
double sqrt(double); double pow(double, double); double fabs(double);
#define INT_MAX 2147483647
#define INT_MIN (-2147483647 - 1)
#define LLONG_MAX 9223372036854775807LL
#define LLONG_MIN (-9223372036854775807LL - 1)
#define NULL 0
)STUB";
  return kPrelude;
}

}  // namespace plag
