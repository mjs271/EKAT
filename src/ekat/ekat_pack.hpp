#ifndef INCLUDE_EKAT_PACK
#define INCLUDE_EKAT_PACK

//TODO
// - bounds checking define

#include "ekat/util/ekat_math_utils.hpp"
#include "ekat/ekat_macros.hpp"
#include "ekat/ekat_scalar_traits.hpp"
#include "ekat/ekat_type_traits.hpp"

#include <Kokkos_Core.hpp>

namespace ekat {

/* API for using "packed" data in ekat. Packs are just bundles of N
   scalars within a single object. Using packed data makes it much easier
   to get good vectorization with C++.

   Pack is a vectorization pack, and Mask is a conditional mask for Pack::set
   constructed from operators among packs.

   If pack size (PACKN) is 1, then a Pack behaves as a scalar, and a Mask
   behaves roughly as a bool. Mask purposely does not support 'operator bool'
   because it is ambiguous whether operator bool should act as any() or all(),
   so we want the caller to be explicit.
 */

template <int PackSize>
struct Mask {
  // One tends to think a short boolean type would be useful here (e.g., bool or
  // char), but that is bad for vectorization. int or long are best.
  typedef long type;

  // A tag for this struct for type checking.
  enum { masktag = true };
  // Pack and Mask sizes are the same, n.
  enum { n = PackSize };

  KOKKOS_FORCEINLINE_FUNCTION
  Mask () {}

  // Init all slots of the Mask to 'init'.
  KOKKOS_FORCEINLINE_FUNCTION explicit Mask (const bool& init) {
    //vector_simd // Intel 18 is having an issue with this loop.
    vector_disabled for (int i = 0; i < n; ++i) d[i] = init;
  }

  // Set slot i to val.
  KOKKOS_FORCEINLINE_FUNCTION void set (const int& i, const bool& val) { d[i] = val; }
  // Get slot i.
  KOKKOS_FORCEINLINE_FUNCTION bool operator[] (const int& i) const { return d[i]; }

  // Is any slot true?
  KOKKOS_FORCEINLINE_FUNCTION bool any () const {
    bool b = false;
    vector_simd for (int i = 0; i < n; ++i) if (d[i]) b = true;
    return b;
  }

  // Are all slots true?
  KOKKOS_FORCEINLINE_FUNCTION bool all () const {
    bool b = true;
    vector_simd for (int i = 0; i < n; ++i) if ( ! d[i]) b = false;
    return b;
  }

  // Are all slots false?
  KOKKOS_FORCEINLINE_FUNCTION bool none () const {
    return !any();
  }

private:
  type d[n];
};

// Use enable_if and masktag so that we can template on 'Mask' and yet not have
// our operator overloads, in particular, be used for something other than the
// Mask type.
template <typename MaskType>
using OnlyMask = typename std::enable_if<MaskType::masktag,MaskType>::type;
template <typename MaskType, typename ReturnType>
using OnlyMaskReturn = typename std::enable_if<MaskType::masktag,ReturnType>::type;

// Codify how a user can construct their own loops conditioned on mask slot
// values.
#define ekat_masked_loop(mask, s)                         \
  vector_simd for (int s = 0; s < mask.n; ++s) if (mask[s])

#define ekat_masked_loop_no_force_vec(mask, s)              \
  vector_ivdep for (int s = 0; s < mask.n; ++s) if (mask[s])

#define ekat_masked_loop_no_vec(mask, s)                    \
  vector_novec for (int s = 0; s < mask.n; ++s) if (mask[s])

// Implementation detail for generating binary ops for mask op mask.
#define ekat_mask_gen_bin_op_mm(op, impl)                   \
  template <typename Mask> KOKKOS_INLINE_FUNCTION             \
  OnlyMask<Mask> operator op (const Mask& a, const Mask& b) { \
    Mask m;                                                   \
    vector_simd for (int i = 0; i < Mask::n; ++i)             \
      m.set(i, a[i] impl b[i]);                               \
    return m;                                                 \
  }

// Implementation detail for generating binary ops for mask op bool.
#define ekat_mask_gen_bin_op_mb(op, impl)                   \
  template <typename Mask> KOKKOS_INLINE_FUNCTION             \
  OnlyMask<Mask> operator op (const Mask& a, const bool b) {  \
    Mask m;                                                   \
    vector_simd for (int i = 0; i < Mask::n; ++i)             \
      m.set(i, a[i] impl b);                                  \
    return m;                                                 \
  }

ekat_mask_gen_bin_op_mm(&&, &&)
ekat_mask_gen_bin_op_mm(||, ||)
ekat_mask_gen_bin_op_mb(&&, &&)
ekat_mask_gen_bin_op_mb(||, ||)

// Negate the mask.
template <typename MaskType> KOKKOS_INLINE_FUNCTION
OnlyMask<MaskType> operator ! (const MaskType& m) {
  MaskType not_m;
  vector_simd for (int i = 0; i < MaskType::n; ++i) not_m.set(i, ! m[i]);
  return not_m;
}

// Implementation detail for generating Pack assignment operators. _p means the
// input is a Pack; _s means the input is a scalar.
#define ekat_pack_gen_assign_op_p(op)                   \
  KOKKOS_FORCEINLINE_FUNCTION                             \
  Pack& operator op (const Pack& a) {                     \
    vector_simd for (int i = 0; i < n; ++i) d[i] op a[i]; \
    return *this;                                         \
  }
#define ekat_pack_gen_assign_op_s(op)                 \
  KOKKOS_FORCEINLINE_FUNCTION                           \
  Pack& operator op (const scalar& a) {                 \
    vector_simd for (int i = 0; i < n; ++i) d[i] op a;  \
    return *this;                                       \
  }
#define ekat_pack_gen_assign_op_all(op)       \
  ekat_pack_gen_assign_op_p(op)               \
  ekat_pack_gen_assign_op_s(op)

// The Pack type. Mask was defined first since it's used in Pack.
template <typename ScalarType, int PackSize>
struct Pack {
  // Pack's tag for type checking.
  enum { packtag = true };
  // Number of slots in the Pack.
  enum { n = PackSize };

  // A definitely non-const version of the input scalar type.
  typedef typename std::remove_const<ScalarType>::type scalar;

  KOKKOS_FORCEINLINE_FUNCTION
  Pack () {
    vector_simd for (int i = 0; i < n; ++i) {
      d[i] = ScalarTraits<scalar>::invalid();
    }
  }

  // Init all slots to scalar v.
  KOKKOS_FORCEINLINE_FUNCTION
  explicit Pack (const scalar& v) {
    vector_simd for (int i = 0; i < n; ++i) d[i] = v;
  }

  // Init this Pack from another one.
  template <typename PackIn, typename = typename std::enable_if<PackIn::packtag>::type>
  KOKKOS_FORCEINLINE_FUNCTION explicit
  Pack (const PackIn& v) {
    static_assert(static_cast<int>(PackIn::n) == static_cast<int>(n),
                  "Pack::n must be the same.");
    vector_simd for (int i = 0; i < n; ++i) d[i] = v[i];
  }

  // Init this Pack from another one, but only where Mask is true; otherwise
  // init to default value.
  template <typename PackIn>
  KOKKOS_FORCEINLINE_FUNCTION
  explicit Pack (const Mask<PackSize>& m, const PackIn& p) {
    static_assert(static_cast<int>(PackIn::n) == PackSize,
                  "Pack::n must be the same.");
    vector_simd for (int i = 0; i < n; ++i) {
      d[i] = m[i] ? p[i] : ScalarTraits<scalar>::invalid();
    }
  }

  KOKKOS_FORCEINLINE_FUNCTION const scalar& operator[] (const int& i) const { return d[i]; }
  KOKKOS_FORCEINLINE_FUNCTION scalar& operator[] (const int& i) { return d[i]; }

  ekat_pack_gen_assign_op_all(=)
  ekat_pack_gen_assign_op_all(+=)
  ekat_pack_gen_assign_op_all(-=)
  ekat_pack_gen_assign_op_all(*=)
  ekat_pack_gen_assign_op_all(/=)

  KOKKOS_FORCEINLINE_FUNCTION
  void set (const Mask<n>& mask, const scalar& v) {
    vector_simd for (int i = 0; i < n; ++i) if (mask[i]) d[i] = v;
  }

  template <typename PackIn>
  KOKKOS_FORCEINLINE_FUNCTION
  void set (const Mask<n>& mask, const PackIn& p,
            typename std::enable_if<PackIn::packtag>::type* = nullptr) {
    static_assert(static_cast<int>(PackIn::n) == PackSize,
                  "Pack::n must be the same.");
    vector_simd for (int i = 0; i < n; ++i) if (mask[i]) d[i] = p[i];
  }

private:
  scalar d[n];
};

// Use enable_if and packtag so that we can template on 'Pack' and yet not have
// our operator overloads, in particular, be used for something other than the
// Pack type.
template <typename PackType>
using OnlyPack = typename std::enable_if<PackType::packtag,PackType>::type;
template <typename PackType, typename ReturnType>
using OnlyPackReturn = typename std::enable_if<PackType::packtag,ReturnType>::type;

// Later, we might support type promotion. For now, caller must explicitly
// promote a pack's scalar type in mixed-type arithmetic.

#define ekat_pack_gen_bin_op_pp(op)                                   \
  template <typename PackType>                                          \
  KOKKOS_FORCEINLINE_FUNCTION                                           \
  OnlyPack<PackType>                                                    \
  operator op (const PackType& a, const PackType& b) {                  \
    PackType c;                                                         \
    vector_simd                                                         \
    for (int i = 0; i < PackType::n; ++i) c[i] = a[i] op b[i];          \
    return c;                                                           \
  }
#define ekat_pack_gen_bin_op_ps(op)                                   \
  template <typename PackType, typename ScalarType>                     \
  KOKKOS_FORCEINLINE_FUNCTION                                           \
  OnlyPack<PackType>                                                    \
  operator op (const PackType& a, const ScalarType& b) {                \
    PackType c;                                                         \
    vector_simd                                                         \
    for (int i = 0; i < PackType::n; ++i) c[i] = a[i] op b;             \
    return c;                                                           \
  }
#define ekat_pack_gen_bin_op_sp(op)                                   \
  template <typename PackType, typename ScalarType>                     \
  KOKKOS_FORCEINLINE_FUNCTION                                           \
  OnlyPack<PackType>                                                    \
  operator op (const ScalarType& a, const PackType& b) {                \
    PackType c;                                                         \
    vector_simd                                                         \
    for (int i = 0; i < PackType::n; ++i) c[i] = a op b[i];             \
    return c;                                                           \
  }
#define ekat_pack_gen_bin_op_all(op)          \
  ekat_pack_gen_bin_op_pp(op)                 \
  ekat_pack_gen_bin_op_ps(op)                 \
  ekat_pack_gen_bin_op_sp(op)

ekat_pack_gen_bin_op_all(+)
ekat_pack_gen_bin_op_all(-)
ekat_pack_gen_bin_op_all(*)
ekat_pack_gen_bin_op_all(/)

#define ekat_pack_gen_unary_op(op)                           \
  template <typename PackType>                                 \
  KOKKOS_FORCEINLINE_FUNCTION                                  \
  OnlyPack<PackType>                                           \
  operator op (const PackType& a) {                            \
    PackType b;                                                \
    vector_simd                                                \
    for (int i = 0; i < PackType::n; ++i) b[i] = op a[i];      \
    return b;                                                  \
  }

ekat_pack_gen_unary_op(-)

#define ekat_pack_gen_unary_fn(fn, impl)                              \
  template <typename PackType>                                        \
  KOKKOS_INLINE_FUNCTION                                              \
  OnlyPack<PackType> fn (const PackType& p) {                         \
    PackType s;                                                       \
    vector_simd                                                       \
    for (int i = 0; i < PackType::n; ++i) s[i] = impl(p[i]);          \
    return s;                                                         \
  }

#define ekat_pack_gen_unary_stdfn(fn) ekat_pack_gen_unary_fn(fn, std::fn)
ekat_pack_gen_unary_stdfn(abs)
ekat_pack_gen_unary_stdfn(exp)
ekat_pack_gen_unary_stdfn(log)
ekat_pack_gen_unary_stdfn(log10)
ekat_pack_gen_unary_stdfn(tgamma)
ekat_pack_gen_unary_stdfn(sqrt)
ekat_pack_gen_unary_stdfn(cbrt)
ekat_pack_gen_unary_stdfn(tanh)

template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar> min (const PackType& p) {
  typename PackType::scalar v(p[0]);
  vector_disabled for (int i = 0; i < PackType::n; ++i) v = impl::min(v, p[i]);
  return v;
}

template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar> max (const PackType& p) {
  typename PackType::scalar v(p[0]);
  vector_simd for (int i = 0; i < PackType::n; ++i) v = impl::max(v, p[i]);
  return v;
}

// sum = sum+p[0]+p[1]+...
// NOTE: f<bool,T> and f<T,bool> are *guaranteed* to be different overloads.
//       The latter is better when bool needs a default, the former is
//       better when bool must be specified, but we want T to be deduced.
template <bool Serialize, typename PackType>
KOKKOS_INLINE_FUNCTION
void reduce_sum (const PackType& p, typename PackType::scalar& sum) {
  if (Serialize) {
    for (int i = 0; i < PackType::n; ++i) sum += p[i];
  } else {
    vector_simd for (int i = 0; i < PackType::n; ++i) sum += p[i];
  }
}

template <typename PackType, bool Serialize = ekatBFB>
KOKKOS_INLINE_FUNCTION
void reduce_sum (const PackType& p, typename PackType::scalar& sum) {
  reduce_sum<Serialize>(p,sum);
}

// return sum = p[0]+p[1]+...
template <bool Serialize, typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar> reduce_sum (const PackType& p) {
  typename PackType::scalar sum = typename PackType::scalar(0);
  reduce_sum<Serialize>(p,sum);
  return sum;
}
template <typename PackType, bool Serialize = ekatBFB>
KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar> reduce_sum (const PackType& p) {
  return reduce_sum<Serialize>(p);
}

// min(init, min(p(mask)))
template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar>
min (const Mask<PackType::n>& mask, typename PackType::scalar init, const PackType& p) {
  vector_disabled for (int i = 0; i < PackType::n; ++i)
    if (mask[i]) init = impl::min(init, p[i]);
  return init;
}

// max(init, max(p(mask)))
template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, typename PackType::scalar>
max (const Mask<PackType::n>& mask, typename PackType::scalar init, const PackType& p) {
  vector_simd for (int i = 0; i < PackType::n; ++i)
    if (mask[i]) init = impl::max(init, p[i]);
  return init;
}

#define ekat_pack_gen_bin_fn_pp(fn, impl)                       \
  template <typename PackType> KOKKOS_INLINE_FUNCTION             \
  OnlyPack<PackType> fn (const PackType& a, const PackType& b) {  \
    PackType s;                                                   \
    vector_simd for (int i = 0; i < PackType::n; ++i)             \
      s[i] = impl(a[i], b[i]);                                    \
    return s;                                                     \
  }
#define ekat_pack_gen_bin_fn_ps(fn, impl)                         \
  template <typename PackType, typename ScalarType>                 \
  KOKKOS_INLINE_FUNCTION                                            \
  OnlyPack<PackType>                                                \
  fn (const PackType& a, const ScalarType& b) {                     \
    PackType s;                                                     \
    vector_simd for (int i = 0; i < PackType::n; ++i)               \
      s[i] = impl<typename PackType::scalar>(a[i], b);              \
    return s;                                                       \
  }
#define ekat_pack_gen_bin_fn_sp(fn, impl)                         \
  template <typename PackType, typename ScalarType>                 \
  KOKKOS_INLINE_FUNCTION                                            \
  OnlyPack<PackType> fn (const ScalarType& a, const PackType& b) {  \
    PackType s;                                                     \
    vector_simd for (int i = 0; i < PackType::n; ++i)               \
      s[i] = impl<typename PackType::scalar>(a, b[i]);              \
    return s;                                                       \
  }
#define ekat_pack_gen_bin_fn_all(fn, impl)    \
  ekat_pack_gen_bin_fn_pp(fn, impl)           \
  ekat_pack_gen_bin_fn_ps(fn, impl)           \
  ekat_pack_gen_bin_fn_sp(fn, impl)

ekat_pack_gen_bin_fn_all(min, impl::min)
ekat_pack_gen_bin_fn_all(max, impl::max)

// On Intel 17 for KNL, I'm getting a ~1-ulp diff on const Scalar& b. I don't
// understand its source. But, in any case, I'm writing a separate impl here to
// get around that.
//ekat_pack_gen_bin_fn_all(pow, std::pow)
template <typename PackType, typename ScalarType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> pow (const PackType& a, const ScalarType/*&*/ b) {
  PackType s;
  vector_simd for (int i = 0; i < PackType::n; ++i)
    s[i] = std::pow(a[i], b);
  return s;
}

template <typename ScalarType, typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> pow (const ScalarType a, const PackType& b) {
  PackType s;
  vector_simd for (int i = 0; i < PackType::n; ++i)
    s[i] = std::pow(a, b[i]);
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> pow (const PackType& a, const PackType& b) {
  PackType s;
  vector_simd for (int i = 0; i < PackType::n; ++i)
    s[i] = std::pow(a[i], b[i]);
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> square (const PackType& a) {
  PackType s;
  vector_simd for (int i = 0; i < PackType::n; ++i)
    s[i] = a[i] * a[i];
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> cube (const PackType& a) {
  PackType s;
  vector_simd for (int i = 0; i < PackType::n; ++i)
    s[i] = a[i] * a[i] * a[i];
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> shift_right (const PackType& pm1, const PackType& p) {
  PackType s;
  s[0] = pm1[PackType::n-1];
  vector_simd for (int i = 1; i < PackType::n; ++i) s[i] = p[i-1];
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> shift_right (const typename PackType::scalar& pm1, const PackType& p) {
  PackType s;
  s[0] = pm1;
  vector_simd for (int i = 1; i < PackType::n; ++i) s[i] = p[i-1];
  return s;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> shift_left (const PackType& pp1, const PackType& p) {
  PackType s;
  s[PackType::n-1] = pp1[0];
  vector_simd for (int i = 0; i < PackType::n-1; ++i) s[i] = p[i+1];
  return s;
}

template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> shift_left (const typename PackType::scalar& pp1, const PackType& p) {
  PackType s;
  s[PackType::n-1] = pp1;
  vector_simd for (int i = 0; i < PackType::n-1; ++i) s[i] = p[i+1];
  return s;
}

#define ekat_mask_gen_bin_op_pp(op)                     \
  template <typename PackType>                            \
  KOKKOS_INLINE_FUNCTION                                  \
  OnlyPackReturn<PackType, Mask<PackType::n> >            \
  operator op (const PackType& a, const PackType& b) {    \
    Mask<PackType::n> m;                           \
    vector_simd for (int i = 0; i < PackType::n; ++i)     \
      m.set(i, a[i] op b[i]);                             \
    return m;                                             \
  }
#define ekat_mask_gen_bin_op_ps(op)                               \
  template <typename PackType, typename ScalarType>                 \
  KOKKOS_INLINE_FUNCTION                                            \
  OnlyPackReturn<PackType, Mask<PackType::n> >                      \
  operator op (const PackType& a, const ScalarType& b) {            \
    Mask<PackType::n> m;                                     \
    vector_simd for (int i = 0; i < PackType::n; ++i)               \
      m.set(i, a[i] op b);                                          \
    return m;                                                       \
  }
#define ekat_mask_gen_bin_op_sp(op)                               \
  template <typename PackType, typename ScalarType>                 \
  KOKKOS_INLINE_FUNCTION                                            \
  OnlyPackReturn<PackType, Mask<PackType::n> >                      \
  operator op (const ScalarType& a, const PackType& b) {            \
    Mask<PackType::n> m;                                     \
    vector_simd for (int i = 0; i < PackType::n; ++i)               \
      m.set(i, a op b[i]);                                          \
    return m;                                                       \
  }
#define ekat_mask_gen_bin_op_all(op)          \
  ekat_mask_gen_bin_op_pp(op)                 \
  ekat_mask_gen_bin_op_ps(op)                 \
  ekat_mask_gen_bin_op_sp(op)

ekat_mask_gen_bin_op_all(==)
ekat_mask_gen_bin_op_all(!=)
ekat_mask_gen_bin_op_all(>=)
ekat_mask_gen_bin_op_all(<=)
ekat_mask_gen_bin_op_all(>)
ekat_mask_gen_bin_op_all(<)

template <typename PackType> KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType, Mask<PackType::n>>
isnan (const PackType& p) {
  Mask<PackType::n> m;
  vector_simd for (int i = 0; i < PackType::n; ++i) {
    m.set(i, impl::is_nan(p[i]));
  }
  return m;
}


template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPackReturn<PackType,Int> npack(const Int& nscalar) {
  return (nscalar + PackType::n - 1) / PackType::n;
}

template <typename PackType>
KOKKOS_INLINE_FUNCTION
OnlyPack<PackType> range (const typename PackType::scalar& start) {
  PackType p;
  vector_simd for (int i = 0; i < PackType::n; ++i) p[i] = start + i;
  return p;
}

#undef ekat_pack_gen_assign_op_p
#undef ekat_pack_gen_assign_op_s
#undef ekat_pack_gen_assign_op_all
#undef ekat_pack_gen_bin_op_pp
#undef ekat_pack_gen_bin_op_ps
#undef ekat_pack_gen_bin_op_sp
#undef ekat_pack_gen_bin_op_all
#undef ekat_pack_gen_unary_op
#undef ekat_pack_gen_unary_fn
#undef ekat_pack_gen_unary_stdfn
#undef ekat_pack_gen_bin_fn_pp
#undef ekat_pack_gen_bin_fn_ps
#undef ekat_pack_gen_bin_fn_sp
#undef ekat_pack_gen_bin_fn_all
#undef ekat_mask_gen_bin_op_pp
#undef ekat_mask_gen_bin_op_ps
#undef ekat_mask_gen_bin_op_sp
#undef ekat_mask_gen_bin_op_all
#undef ekat_mask_gen_bin_op_mm
#undef ekat_mask_gen_bin_op_mb

// Specialization of ScalarTraits struct for Pack types

template<typename T, int N>
struct ScalarTraits<Pack<T,N>> {

  using inner_traits = ScalarTraits<T>;

  using value_type  = Pack<T,N>;
  using scalar_type = typename inner_traits::scalar_type;

  // TODO: should we allow a pack of packs? For now, I assume the answer is NO.
  //       So in order to FORBID pack<pack<T,N>>, check that inner_traits::value_type
  //       is an arithmetic type.
  static_assert (std::is_arithmetic<typename inner_traits::value_type>::value,
                 "Error! We do not allow nested pack structures, for now.\n");

  // This seems funky. But write down a pow of 2 and a non-pow of 2 in binary (both positive), and you'll see why it works
  static_assert (N>0 && ((N & (N-1))==0), "Error! We only support packs with length = 2^n.\n");

  // Roll our own (hopefully verbose enough) name for this type.
  static std::string name () {
    return "Pack<" + inner_traits::name() + "," + std::to_string(N) + ">";
  }
  static constexpr bool is_simd = true;

  KOKKOS_INLINE_FUNCTION
  static const value_type quiet_NaN () {
    return value_type(inner_traits::quiet_NaN());
  }

  KOKKOS_INLINE_FUNCTION
  static const value_type invalid () {
    return value_type(inner_traits::invalid());
  }
};

} // namespace ekat

#endif // INCLUDE_EKAT_PACK
