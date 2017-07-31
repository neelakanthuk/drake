#pragma once

#include <map>
#include <memory>
#include <typeindex>
#include <utility>

#include "drake/common/drake_assert.h"
#include "drake/common/drake_copyable.h"
#include "drake/common/eigen_autodiff_types.h"
#include "drake/common/symbolic.h"
#include "drake/systems/framework/scalar_conversion_traits.h"
#include "drake/systems/framework/system.h"
#include "drake/systems/framework/system_type_tag.h"

namespace drake {
namespace systems {

/// Helper class to convert a System<U> into a System<T>, intended for internal
/// use by the System framework, not directly by users.
///
/// Because it is not templated on a System subclass, this class can be used by
/// LeafSystem without any direct knowledge of the subtypes being converted.
/// In other words, it enables a runtime flavor of the CRTP.
class SystemScalarConverter {
 public:
  DRAKE_DEFAULT_COPY_AND_MOVE_AND_ASSIGN(SystemScalarConverter);

  /// Creates an object that returns nullptr for all Convert() requests.  The
  /// single-argument constructor below is the typical way to create a useful
  /// instance of this type.
  SystemScalarConverter() = default;

  /// Creates an object that uses S's scalar-type converting copy constructor.
  /// That constructor takes the form of, e.g.:
  ///
  /// @code
  /// template <typename T>
  /// class Foo {
  ///   template <typename U>
  ///   explicit Foo(const Foo<U>& other);
  /// };
  /// @endcode
  ///
  /// This constructor only creates a converter between a limited set of types,
  /// specifically:
  ///
  /// - double
  /// - AutoDiffXd
  /// - symbolic::Expression
  ///
  /// By default, all non-identity pairs (pairs where T and U differ) drawn
  /// from the above list can be used for T and U.  Systems may specialize
  /// scalar_conversion::Traits to disable support for some or all of these
  /// conversions, or after construction may call Add<T, U>() on the returned
  /// object to enable support for additional custom types.
  ///
  /// @tparam S is the System type to convert
  template <template <typename> class S>
  explicit SystemScalarConverter(SystemTypeTag<S>) : SystemScalarConverter() {
    using Expression = symbolic::Expression;
    // From double to all other types.
    AddIfSupported<S, AutoDiffXd, double>();
    AddIfSupported<S, Expression, double>();
    // From AutoDiffXd to all other types.
    AddIfSupported<S, double,     AutoDiffXd>();
    AddIfSupported<S, Expression, AutoDiffXd>();
    // From Expression to all other types.
    AddIfSupported<S, double,     Expression>();
    AddIfSupported<S, AutoDiffXd, Expression>();
  }

  /// A std::function used to convert a System<U> into a System<T>.
  template <typename T, typename U>
  using ConverterFunction =
      std::function<std::unique_ptr<System<T>>(const System<U>&)>;

  /// Registers the std::function to be used to convert a System<U> into a
  /// System<T>.  A pair of types can be registered (added) at most once.
  template <typename T, typename U>
  void Add(const ConverterFunction<T, U>&);

  /// Adds converter for an S<U> into an S<T>, iff scalar_conversion::Traits
  /// says its supported.  The converter uses S's scalar-type converting copy
  /// constructor.
  template <template <typename> class S, typename T, typename U>
  void AddIfSupported();

  /// Returns true iff this object can convert a System<U> into a System<T>,
  /// i.e., whether Convert() will return non-null.
  ///
  /// @tparam U the donor scalar type (to convert from)
  /// @tparam T the resulting scalar type (to convert into)
  template <typename T, typename U>
  bool IsConvertible() const;

  /// Converts a System<U> into a System<T>.  This is the API that LeafSystem
  /// uses to provide a default implementation of DoToAutoDiffXd, etc.
  ///
  /// @tparam U the donor scalar type (to convert from)
  /// @tparam T the resulting scalar type (to convert into)
  template <typename T, typename U>
  std::unique_ptr<System<T>> Convert(const System<U>& other) const;

 private:
  // Like ConverterFunc, but with the args and return value decayed into void*.
  using ErasedConverterFunc = std::function<void*(const void*)>;

  // Given typeid(T), typeid(U), returns a converter.  If no converter has been
  // added yet, returns nullptr.
  const ErasedConverterFunc* Find(
      const std::type_info&, const std::type_info&) const;

  // Given typeid(T), typeid(U), adds a converter.
  void Insert(
      const std::type_info&, const std::type_info&,
      const ErasedConverterFunc&);

  // Maps from {T, U} to the function that converts from U into T.
  std::map<std::pair<std::type_index, std::type_index>, ErasedConverterFunc>
      funcs_;
};

#if !defined(DRAKE_DOXYGEN_CXX)

template <typename T, typename U>
void SystemScalarConverter::Add(const ConverterFunction<T, U>& func) {
  // Make sure func contains a target (i.e., is not null-ish).
  DRAKE_ASSERT(static_cast<bool>(func));
  // Copy `func` into a lambda that ends up stored into `funcs_`.  The lambda
  // is typed as `void* => void*` in order to have a non-templated signature
  // and thus fit into a homogeneously-typed std::map.
  Insert(typeid(T), typeid(U), [func](const void* const bare_u) {
    DRAKE_ASSERT(bare_u);
    const System<U>& other = *static_cast<const System<U>*>(bare_u);
    return func(other).release();
  });
}

template <typename T, typename U>
bool SystemScalarConverter::IsConvertible() const {
  const ErasedConverterFunc* converter = Find(typeid(T), typeid(U));
  return (converter != nullptr);
}

template <typename T, typename U>
std::unique_ptr<System<T>> SystemScalarConverter::Convert(
    const System<U>& other) const {
  // Lookup the lambda that Add() stored and call it.
  System<T>* result = nullptr;
  const ErasedConverterFunc* converter = Find(typeid(T), typeid(U));
  if (converter) {
    result = static_cast<System<T>*>((*converter)(&other));
  }
  return std::unique_ptr<System<T>>(result);
}

namespace system_scalar_converter_detail {
// When Traits says that conversion is supported.
template <template <typename> class S, typename T, typename U>
static std::unique_ptr<System<T>> Make(
    const System<U>& other, std::true_type) {
  const auto& my_other = dynamic_cast<const S<U>&>(other);
  return std::make_unique<S<T>>(my_other);
}
// When Traits says not to convert.
template <template <typename> class S, typename T, typename U>
static std::unique_ptr<System<T>> Make(
    const System<U>&, std::false_type) {
  // AddIfSupported is guaranteed not to call us, but we *will* be compiled,
  // so we have to have some kind of function body.
  DRAKE_ABORT();
}
}  // namespace system_scalar_converter_detail

template <template <typename> class S, typename T, typename U>
void SystemScalarConverter::AddIfSupported() {
  using supported =
      typename scalar_conversion::Traits<S>::template supported<T, U>;
  if (supported::value) {
    const ConverterFunction<T, U> func = [](const System<U>& other) {
      // Dispatch to an overload based on whether S<U> ==> S<T> is supported.
      // (At runtime, this block is only executed for supported conversions,
      // but at compile time, Make will be instantiated unconditionally.)
      return system_scalar_converter_detail::Make<S, T, U>(other, supported{});
    };
    Add(func);
  }
}

#endif  // DRAKE_DOXYGEN_CXX

}  // namespace systems
}  // namespace drake