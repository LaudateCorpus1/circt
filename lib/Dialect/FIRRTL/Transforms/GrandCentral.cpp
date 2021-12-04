//===- GrandCentral.cpp - Ingest black box sources --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//
//
// Implement SiFive's Grand Central transform.  Currently, this supports
// SystemVerilog Interface generation.
//
//===----------------------------------------------------------------------===//

#include "../AnnotationDetails.h"
#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotationLowering.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/InstanceGraph.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "circt/Dialect/HW/HWOps.h"
#include "circt/Dialect/SV/SVOps.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/YAMLTraits.h"
#include <variant>

#define DEBUG_TYPE "gct"

using namespace circt;
using namespace firrtl;
using llvm::Optional;

//===----------------------------------------------------------------------===//
// Collateral for generating a YAML representation of a SystemVerilog interface
//===----------------------------------------------------------------------===//

namespace {

// These macros are used to provide hard-errors if a user tries to use the YAML
// infrastructure improperly.  We only implement conversion to YAML and not
// conversion from YAML.  The LLVM YAML infrastructure doesn't provide the
// ability to differentitate this and we don't need it for the purposes of
// Grand Central.
#define UNIMPLEMENTED_DEFAULT(clazz)                                           \
  llvm_unreachable("default '" clazz                                           \
                   "' construction is an intentionally *NOT* implemented "     \
                   "YAML feature (you should never be using this)");
#define UNIMPLEMENTED_DENORM(clazz)                                            \
  llvm_unreachable("conversion from YAML to a '" clazz                         \
                   "' is intentionally *NOT* implemented (you should not be "  \
                   "converting from YAML to an interface)");

// This namespace provides YAML-related collateral that is specific to Grand
// Central and should not be placed in the `llvm::yaml` namespace.
namespace yaml {

/// Context information necessary for YAML generation.
struct Context {
  /// A symbol table consisting of _only_ the interfaces construted by the Grand
  /// Central pass.  This is not a symbol table because we do not have an
  /// up-to-date symbol table that includes interfaces at the time the Grand
  /// Central pass finishes.  This structure is easier to build up and is only
  /// the information we need.
  DenseMap<Attribute, sv::InterfaceOp> &interfaceMap;
};

/// A representation of an `sv::InterfaceSignalOp` that includes additional
/// description information.
///
/// TODO: This could be removed if we add `firrtl.DocStringAnnotation` support
/// or if FIRRTL dialect included support for ops to specify "comment"
/// information.
struct DescribedSignal {
  /// The comment associated with this signal.
  StringAttr description;

  /// The signal.
  sv::InterfaceSignalOp signal;
};

/// This exist to work around the fact that no interface can be instantiated
/// inside another interface.  This serves to represent an op like this for the
/// purposes of conversion to YAML.
///
/// TODO: Fix this once we have a solution for #1464.
struct DescribedInstance {
  StringAttr name;

  /// A comment associated with the interface instance.
  StringAttr description;

  /// The dimensionality of the interface instantiation.
  ArrayAttr dimensions;

  /// The symbol associated with the interface.
  FlatSymbolRefAttr interface;
};

} // namespace yaml
} // namespace

// These macros tell the YAML infrastructure that these are types which can
// show up in vectors and provides implementations of how to serialize these.
// Each of these macros puts the resulting class into the `llvm::yaml` namespace
// (which is why these are outside the `llvm::yaml` namespace below).
LLVM_YAML_IS_SEQUENCE_VECTOR(::yaml::DescribedSignal)
LLVM_YAML_IS_SEQUENCE_VECTOR(::yaml::DescribedInstance)
LLVM_YAML_IS_SEQUENCE_VECTOR(sv::InterfaceOp)

// This `llvm::yaml` namespace contains implementations of classes that enable
// conversion from an `sv::InterfaceOp` to a YAML representation of that
// interface using [LLVM's YAML I/O library](https://llvm.org/docs/YamlIO.html).
namespace llvm {
namespace yaml {

using namespace ::yaml;

/// Conversion from a `DescribedSignal` to YAML.  This is
/// implemented using YAML normalization to first convert this to an internal
/// `Field` structure which has a one-to-one mapping to the YAML represntation.
template <>
struct MappingContextTraits<DescribedSignal, Context> {
  /// A one-to-one representation with a YAML representation of a signal/field.
  struct Field {
    /// The name of the field.
    StringRef name;

    /// An optional, textual description of what the field is.
    Optional<StringRef> description;

    /// The dimensions of the field.
    SmallVector<unsigned, 2> dimensions;

    /// The width of the underlying type.
    unsigned width;

    /// Construct a `Field` from a `DescribedSignal` (an `sv::InterfaceSignalOp`
    /// with an optional description).
    Field(IO &io, DescribedSignal &op)
        : name(op.signal.sym_nameAttr().getValue()) {

      // Convert the description from a `StringAttr` (which may be null) to an
      // `Optional<StringRef>`.  This aligns exactly with the YAML
      // representation.
      if (op.description) {
        description = op.description.getValue();
        description->consume_front("// ");
      }

      // Unwrap the type of the field into an array of dimensions and a width.
      // By example, this is going from the following hardware type:
      //
      //     !hw.uarray<1xuarray<2xuarray<3xi8>>>
      //
      // To the following representation:
      //
      //     dimensions: [ 3, 2, 1 ]
      //     width: 8
      //
      // Note that the above is equivalenet to the following Verilog
      // specification.
      //
      //     wire [7:0] foo [2:0][1:0][0:0]
      //
      // Do this by repeatedly unwrapping unpacked array types until you get to
      // the underlying type.  The dimensions need to be reversed as this
      // unwrapping happens in reverse order of the final representation.
      auto tpe = op.signal.type();
      while (auto vector = tpe.dyn_cast<hw::UnpackedArrayType>()) {
        dimensions.push_back(vector.getSize());
        tpe = vector.getElementType();
      }
      dimensions = SmallVector<unsigned>(llvm::reverse(dimensions));

      // The final non-array type must be an integer.  Leave this as an assert
      // with a blind cast because we generated this type in this pass (and we
      // therefore cannot fail this cast).
      assert(tpe.isa<IntegerType>());
      width = tpe.cast<IntegerType>().getWidth();
    }

    /// A no-argument constructor is necessary to work with LLVM's YAML library.
    Field(IO &io){UNIMPLEMENTED_DEFAULT("Field")}

    /// This cannot be denomralized back to an interface op.
    DescribedSignal denormalize(IO &) {
      UNIMPLEMENTED_DENORM("DescribedSignal")
    }
  };

  static void mapping(IO &io, DescribedSignal &op, Context &ctx) {
    MappingNormalization<Field, DescribedSignal> keys(io, op);
    io.mapRequired("name", keys->name);
    io.mapOptional("description", keys->description);
    io.mapRequired("dimensions", keys->dimensions);
    io.mapRequired("width", keys->width);
  }
};

/// Conversion from a `DescribedInstance` to YAML.  This is implemented using
/// YAML normalization to first convert the `DescribedInstance` to an internal
/// `Instance` struct which has a one-to-one representation with the final YAML
/// representation.
template <>
struct MappingContextTraits<DescribedInstance, Context> {
  /// A YAML-serializable representation of an interface instantiation.
  struct Instance {
    /// The name of the interface.
    StringRef name;

    /// An optional textual description of the interface.
    Optional<StringRef> description = None;

    /// An array describing the dimnensionality of the interface.
    SmallVector<int64_t, 2> dimensions;

    /// The underlying interface.
    FlatSymbolRefAttr interface;

    Instance(IO &io, DescribedInstance &op)
        : name(op.name.getValue()), interface(op.interface) {

      // Convert the description from a `StringAttr` (which may be null) to an
      // `Optional<StringRef>`.  This aligns exactly with the YAML
      // representation.
      if (op.description) {
        description = op.description.getValue();
        description->consume_front("// ");
      }

      for (auto &d : op.dimensions) {
        auto dimension = d.dyn_cast<IntegerAttr>();
        dimensions.push_back(dimension.getInt());
      }
    }

    Instance(IO &io){UNIMPLEMENTED_DEFAULT("Instance")}

    DescribedInstance denormalize(IO &) {
      UNIMPLEMENTED_DENORM("DescribedInstance")
    }
  };

  static void mapping(IO &io, DescribedInstance &op, Context &ctx) {
    MappingNormalization<Instance, DescribedInstance> keys(io, op);
    io.mapRequired("name", keys->name);
    io.mapOptional("description", keys->description);
    io.mapRequired("dimensions", keys->dimensions);
    io.mapRequired("interface", ctx.interfaceMap[keys->interface], ctx);
  }
};

/// Conversion from an `sv::InterfaceOp` to YAML.  This is implemented using
/// YAML normalization to first convert the interface to an internal `Interface`
/// which reformats the Grand Central-generated interface into the YAML format.
template <>
struct MappingContextTraits<sv::InterfaceOp, Context> {
  /// A YAML-serializable representation of an interface.  This consists of
  /// fields (vector or ground types) and nested interfaces.
  struct Interface {
    /// The name of the interface.
    StringRef name;

    /// All ground or vectors that make up the interface.
    std::vector<DescribedSignal> fields;

    /// Instantiations of _other_ interfaces.
    std::vector<DescribedInstance> instances;

    /// Construct an `Interface` from an `sv::InterfaceOp`.  This is tuned to
    /// "parse" the structure of an interface that the Grand Central pass
    /// generates.  The structure of `Field`s and `Instance`s is documented
    /// below.
    ///
    /// A field will look like the following.  The verbatim description is
    /// optional:
    ///
    ///     sv.verbatim "// <description>" {
    ///       firrtl.grandcentral.yaml.type = "description",
    ///       symbols = []}
    ///     sv.interface.signal @<name> : <type>
    ///
    /// An interface instanctiation will look like the following.  The verbatim
    /// description is optional.
    ///
    ///     sv.verbatim "// <description>" {
    ///       firrtl.grandcentral.type = "description",
    ///       symbols = []}
    ///     sv.verbatim "<name> <symbol>();" {
    ///       firrtl.grandcentral.yaml.name = "<name>",
    ///       firrtl.grandcentral.yaml.dimensions = [<first dimension>, ...],
    ///       firrtl.grandcentral.yaml.symbol = @<symbol>,
    ///       firrtl.grandcentral.yaml.type = "instance",
    ///       symbols = []}
    ///
    Interface(IO &io, sv::InterfaceOp &op) : name(op.getName()) {
      // A mutable store of the description.  This occurs in the op _before_ the
      // field or instance, so we need someplace to put it until we use it.
      StringAttr description = {};

      for (auto &op : op.getBodyBlock()->getOperations()) {
        TypeSwitch<Operation *>(&op)
            // A verbatim op is either a description or an interface
            // instantiation.
            .Case<sv::VerbatimOp>([&](sv::VerbatimOp op) {
              auto tpe = op->getAttrOfType<StringAttr>(
                  "firrtl.grandcentral.yaml.type");

              // This is a descripton.  Update the mutable description and
              // continue;
              if (tpe.getValue() == "description") {
                description = op.stringAttr();
                return;
              }

              // This is an unsupported construct. Just drop it.
              if (tpe.getValue() == "unsupported") {
                description = {};
                return;
              }

              // This is an instance of another interface.  Add the symbol to
              // the vector of instances.
              auto name = op->getAttrOfType<StringAttr>(
                  "firrtl.grandcentral.yaml.name");
              auto dimensions = op->getAttrOfType<ArrayAttr>(
                  "firrtl.grandcentral.yaml.dimensions");
              auto symbol = op->getAttrOfType<FlatSymbolRefAttr>(
                  "firrtl.grandcentral.yaml.symbol");
              instances.push_back(
                  DescribedInstance({name, description, dimensions, symbol}));
            })
            // An interface signal op is a field.
            .Case<sv::InterfaceSignalOp>([&](sv::InterfaceSignalOp op) {
              fields.push_back(DescribedSignal({description, op}));
              description = {};
            });
      }
    }

    /// A no-argument constructor is necessary to work with LLVM's YAML library.
    Interface(IO &io){UNIMPLEMENTED_DEFAULT("Interface")}

    /// This cannot be denomralized back to an interface op.
    sv::InterfaceOp denormalize(IO &) {
      UNIMPLEMENTED_DENORM("sv::InterfaceOp")
    }
  };

  static void mapping(IO &io, sv::InterfaceOp &op, Context &ctx) {
    MappingNormalization<Interface, sv::InterfaceOp> keys(io, op);
    io.mapRequired("name", keys->name);
    io.mapRequired("fields", keys->fields, ctx);
    io.mapRequired("instances", keys->instances, ctx);
  }
};

} // namespace yaml
} // namespace llvm

//===----------------------------------------------------------------------===//
// Pass Implementation
//===----------------------------------------------------------------------===//

namespace {

/// A helper to build verbatim strings with symbol placeholders. Provides a
/// mechanism to snapshot the current string and symbols and restore back to
/// this state after modifications. These snapshots are particularly useful when
/// the string is assembled through hierarchical traversal of some sort, which
/// populates the string with a prefix common to all children of a hierarchy
/// (like the interface field traversal in the `GrandCentralPass`).
///
/// The intended use is as follows:
///
///     void baz(VerbatimBuilder &v) {
///       foo(v.snapshot().append("bar"));
///     }
///
/// The function `baz` takes a snapshot of the current verbatim text `v`, adds
/// "bar" to it and calls `foo` with that appended verbatim text. After the call
/// to `foo` returns, any changes made by `foo` as well as the "bar" are dropped
/// from the verbatim text `v`, as the temporary snapshot goes out of scope.
struct VerbatimBuilder {
  struct Base {
    SmallString<128> string;
    SmallVector<Attribute> symbols;
    VerbatimBuilder builder() { return VerbatimBuilder(*this); }
    operator VerbatimBuilder() { return builder(); }
  };

  /// Constructing a builder will snapshot the `Base` which holds the actual
  /// string and symbols.
  VerbatimBuilder(Base &base)
      : base(base), stringBaseSize(base.string.size()),
        symbolsBaseSize(base.symbols.size()) {}

  /// Destroying a builder will reset the `Base` to the original string and
  /// symbols.
  ~VerbatimBuilder() {
    base.string.resize(stringBaseSize);
    base.symbols.resize(symbolsBaseSize);
  }

  // Disallow copying.
  VerbatimBuilder(const VerbatimBuilder &) = delete;
  VerbatimBuilder &operator=(const VerbatimBuilder &) = delete;

  /// Take a snapshot of the current string and symbols. This returns a new
  /// `VerbatimBuilder` that will reset to the current state of the string once
  /// destroyed.
  VerbatimBuilder snapshot() { return VerbatimBuilder(base); }

  /// Get the current string.
  StringRef getString() const { return base.string; }
  /// Get the current symbols;
  ArrayRef<Attribute> getSymbols() const { return base.symbols; }

  /// Append to the string.
  VerbatimBuilder &append(char c) {
    base.string.push_back(c);
    return *this;
  }

  /// Append to the string.
  VerbatimBuilder &append(const Twine &twine) {
    twine.toVector(base.string);
    return *this;
  }

  /// Append a placeholder and symbol to the string.
  VerbatimBuilder &append(Attribute symbol) {
    unsigned id = base.symbols.size();
    base.symbols.push_back(symbol);
    append("{{" + Twine(id) + "}}");
    return *this;
  }

  VerbatimBuilder &operator+=(char c) { return append(c); }
  VerbatimBuilder &operator+=(const Twine &twine) { return append(twine); }
  VerbatimBuilder &operator+=(Attribute symbol) { return append(symbol); }

private:
  Base &base;
  size_t stringBaseSize;
  size_t symbolsBaseSize;
};

/// A wrapper around a string that is used to encode a type which cannot be
/// represented by an mlir::Type for some reason.  This is currently used to
/// represent either an interface, a n-dimensional vector of interfaces, or a
/// tombstone for an actually unsupported type (e.g., an AugmentedBooleanType).
struct VerbatimType {
  /// The textual representation of the type.
  std::string str;

  /// True if this is a type which must be "instatiated" and requires a trailing
  /// "()".
  bool instantiation;

  /// A vector storing the width of each dimension of the type.
  SmallVector<int32_t, 4> dimensions = {};

  /// Serialize this type to a string.
  std::string toStr(StringRef name) {
    SmallString<64> stringType(str);
    stringType.append(" ");
    stringType.append(name);
    for (auto d : llvm::reverse(dimensions)) {
      stringType.append("[");
      stringType.append(Twine(d).str());
      stringType.append("]");
    }
    if (instantiation)
      stringType.append("()");
    stringType.append(";");
    return std::string(stringType);
  }
};

/// A sum type representing either a type encoded as a string (VerbatimType)
/// or an actual mlir::Type.
typedef std::variant<VerbatimType, Type> TypeSum;

/// Stores the information content of an ExtractGrandCentralAnnotation.
struct ExtractionInfo {
  /// The directory where Grand Central generated collateral (modules,
  /// interfaces, etc.) will be written.
  StringAttr directory = {};

  /// The name of the file where any binds will be written.  This will be placed
  /// in the same output area as normal compilation output, e.g., output
  /// Verilog.  This has no relation to the `directory` member.
  StringAttr bindFilename = {};
};

/// Stores information about the companion module of a GrandCentral view.
struct CompanionInfo {
  StringRef name;

  FModuleOp companion;

  FModuleOp mapping;
};

/// Generate SystemVerilog interfaces from Grand Central annotations.  This pass
/// roughly works in the following three phases:
///
/// 1. Extraction information is determined.
///
/// 2. The circuit is walked to find all scattered annotations related to Grand
///    Central interfaces.  These are: (a) the parent module, (b) the companion
///    module, and (c) all leaves that are to be connected to the interface.
///
/// 3. The circuit-level Grand Central annotation is walked to both generate and
///    instantiate interfaces and to generate the "mappings" file that produces
///    cross-module references (XMRs) to drive the interface.
struct GrandCentralPass : public GrandCentralBase<GrandCentralPass> {
  void runOnOperation() override;

private:
  /// Optionally build an AugmentedType from an attribute.  Return none if the
  /// attribute is not a dictionary or if it does not match any of the known
  /// templates for AugmentedTypes.
  Optional<Attribute> fromAttr(Attribute attr);

  /// Mapping of ID to leaf ground type associated with that ID.
  DenseMap<Attribute, Value> leafMap;

  /// Mapping of ID to parent instance and module.  If this module is the top
  /// module, then the first tuple member will be None.
  DenseMap<Attribute, std::pair<Optional<InstanceOp>, FModuleOp>> parentIDMap;

  /// Mapping of ID to companion module.
  DenseMap<Attribute, CompanionInfo> companionIDMap;

  /// An optional prefix applied to all interfaces in the design.  This is set
  /// based on a PrefixInterfacesAnnotation.
  StringRef interfacePrefix;

  /// Return a string containing the name of an interface.  Apply correct
  /// prefixing from the interfacePrefix and module-level prefix parameter.
  std::string getInterfaceName(StringAttr prefix,
                               AugmentedBundleTypeAttr bundleType) {

    if (prefix)
      return (prefix.getValue() + interfacePrefix +
              bundleType.getDefName().getValue())
          .str();
    return (interfacePrefix + bundleType.getDefName().getValue()).str();
  }

  /// Recursively examine an AugmentedType to populate the "mappings" file
  /// (generate XMRs) for this interface.  This does not build new interfaces.
  bool traverseField(Attribute field, IntegerAttr id, VerbatimBuilder &path);

  /// Recursively examine an AugmentedType to both build new interfaces and
  /// populate a "mappings" file (generate XMRs) using `traverseField`.  Return
  /// the type of the field exmained.
  Optional<TypeSum> computeField(Attribute field, IntegerAttr id,
                                 StringAttr prefix, VerbatimBuilder &path);

  /// Recursively examine an AugmentedBundleType to both build new interfaces
  /// and populate a "mappings" file (generate XMRs).  Return none if the
  /// interface is invalid.
  Optional<sv::InterfaceOp> traverseBundle(AugmentedBundleTypeAttr bundle,
                                           IntegerAttr id, StringAttr prefix,
                                           VerbatimBuilder &path);

  /// Return the module associated with this value.
  FModuleLike getEnclosingModule(Value value);

  /// Inforamtion about how the circuit should be extracted.  This will be
  /// non-empty if an extraction annotation is found.
  Optional<ExtractionInfo> maybeExtractInfo = None;

  /// A filename describing where to put a YAML representation of the
  /// interfaces generated by this pass.
  Optional<StringAttr> maybeHierarchyFileYAML = None;

  StringAttr getOutputDirectory() {
    if (maybeExtractInfo.hasValue())
      return maybeExtractInfo.getValue().directory;
    return {};
  }

  /// Store of an instance paths analysis.  This is constructed inside
  /// `runOnOperation`, to work around the deleted copy constructor of
  /// `InstancePathCache`'s internal `BumpPtrAllocator`.
  ///
  /// TODO: Investigate a way to not use a pointer here like how `getNamespace`
  /// works below.
  InstancePathCache *instancePaths = nullptr;

  /// The namespace associated with the circuit.  This is lazily constructed
  /// using `getNamesapce`.
  Optional<CircuitNamespace> circuitNamespace = None;

  /// The module namespaces. These are lazily constructed by
  /// `getModuleNamespace`.
  DenseMap<Operation *, ModuleNamespace> moduleNamespaces;

  /// Return a reference to the circuit namespace.  This will lazily construct a
  /// namespace if one does not exist.
  CircuitNamespace &getNamespace() {
    if (!circuitNamespace)
      circuitNamespace = CircuitNamespace(getOperation());
    return circuitNamespace.getValue();
  }

  /// Get the cached namespace for a module.
  ModuleNamespace &getModuleNamespace(FModuleLike module) {
    auto it = moduleNamespaces.find(module);
    if (it != moduleNamespaces.end())
      return it->second;
    return moduleNamespaces.insert({module, ModuleNamespace(module)})
        .first->second;
  }

  /// A symbol table associated with the circuit.  This is lazily constructed by
  /// `getSymbolTable`.
  Optional<SymbolTable> symbolTable = None;

  /// Return a reference to a circuit-level symbol table.  Lazily construct one
  /// if such a symbol table does not already exist.
  SymbolTable &getSymbolTable() {
    if (!symbolTable)
      symbolTable = SymbolTable(getOperation());
    return symbolTable.getValue();
  }

  // Utility that acts like emitOpError, but does _not_ include a note.  The
  // note in emitOpError includes the entire op which means the **ENTIRE**
  // FIRRTL circuit.  This doesn't communicate anything useful to the user
  // other than flooding their terminal.
  InFlightDiagnostic emitCircuitError(StringRef message = {}) {
    return emitError(getOperation().getLoc(), "'firrtl.circuit' op " + message);
  }

  // Insert comment delimiters ("// ") after newlines in the description string.
  // This is necessary to prevent introducing invalid verbatim Verilog.
  //
  // TODO: Add a comment op and lower the description to that.
  // TODO: Tracking issue: https://github.com/llvm/circt/issues/1677
  std::string cleanupDescription(StringRef description) {
    StringRef head;
    SmallString<64> out;
    do {
      std::tie(head, description) = description.split("\n");
      out.append(head);
      if (!description.empty())
        out.append("\n// ");
    } while (!description.empty());
    return std::string(out);
  }

  /// A store of the YAML representation of interfaces.
  DenseMap<Attribute, sv::InterfaceOp> interfaceMap;

  /// Returns an operation's `inner_sym`, adding one if necessary.
  StringAttr getOrAddInnerSym(Operation *op);

  /// Returns a port's `inner_sym`, adding one if necessary.
  StringAttr getOrAddInnerSym(FModuleLike module, size_t portIdx);

  /// Obtain an inner reference to an operation, possibly adding an `inner_sym`
  /// to that operation.
  hw::InnerRefAttr getInnerRefTo(Operation *op);

  /// Obtain an inner reference to a module port, possibly adding an `inner_sym`
  /// to that port.
  hw::InnerRefAttr getInnerRefTo(FModuleLike module, size_t portIdx);
};

} // namespace

Optional<Attribute> GrandCentralPass::fromAttr(Attribute attr) {
  auto dict = attr.dyn_cast<DictionaryAttr>();
  if (!dict) {
    emitCircuitError() << "attribute is not a dictionary: " << attr << "\n";
    return None;
  }

  auto clazz = dict.getAs<StringAttr>("class");
  if (!clazz) {
    emitCircuitError() << "missing 'class' key in " << dict << "\n";
    return None;
  }

  auto classBase = clazz.getValue();
  classBase.consume_front("sifive.enterprise.grandcentral.Augmented");

  if (classBase == "BundleType") {
    if (dict.getAs<StringAttr>("defName") && dict.getAs<ArrayAttr>("elements"))
      return AugmentedBundleTypeAttr::get(&getContext(), dict);
    emitCircuitError() << "has an invalid AugmentedBundleType that does not "
                          "contain 'defName' and 'elements' fields: "
                       << dict;
  } else if (classBase == "VectorType") {
    if (dict.getAs<StringAttr>("name") && dict.getAs<ArrayAttr>("elements"))
      return AugmentedVectorTypeAttr::get(&getContext(), dict);
    emitCircuitError() << "has an invalid AugmentedVectorType that does not "
                          "contain 'name' and 'elements' fields: "
                       << dict;
  } else if (classBase == "GroundType") {
    auto id = dict.getAs<IntegerAttr>("id");
    auto name = dict.getAs<StringAttr>("name");
    if (id && leafMap.count(id) && name)
      return AugmentedGroundTypeAttr::get(&getContext(), dict);
    if (!id || !name)
      emitCircuitError() << "has an invalid AugmentedGroundType that does not "
                            "contain 'id' and 'name' fields:  "
                         << dict;
    if (id && !leafMap.count(id))
      emitCircuitError() << "has an AugmentedGroundType with 'id == "
                         << id.getValue().getZExtValue()
                         << "' that does not have a scattered leaf to connect "
                            "to in the circuit "
                            "(was the leaf deleted or constant prop'd away?)";
  } else if (classBase == "StringType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedStringTypeAttr::get(&getContext(), dict);
  } else if (classBase == "BooleanType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedBooleanTypeAttr::get(&getContext(), dict);
  } else if (classBase == "IntegerType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedIntegerTypeAttr::get(&getContext(), dict);
  } else if (classBase == "DoubleType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedDoubleTypeAttr::get(&getContext(), dict);
  } else if (classBase == "LiteralType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedLiteralTypeAttr::get(&getContext(), dict);
  } else if (classBase == "DeletedType") {
    if (auto name = dict.getAs<StringAttr>("name"))
      return AugmentedDeletedTypeAttr::get(&getContext(), dict);
  } else {
    emitCircuitError() << "has an invalid AugmentedType";
  }
  return None;
}

bool GrandCentralPass::traverseField(Attribute field, IntegerAttr id,
                                     VerbatimBuilder &path) {
  return TypeSwitch<Attribute, bool>(field)
      .Case<AugmentedGroundTypeAttr>([&](auto ground) {
        Value leafValue = leafMap.lookup(ground.getID());
        assert(leafValue && "leafValue not found");

        auto builder =
            OpBuilder::atBlockEnd(companionIDMap.lookup(id).mapping.getBody());

        auto enclosing = getEnclosingModule(leafValue);
        auto srcPaths = instancePaths->getAbsolutePaths(enclosing);
        assert(srcPaths.size() == 1 &&
               "Unable to handle multiply instantiated companions");

        // Add the root module.
        path += " = ";
        path += FlatSymbolRefAttr::get(SymbolTable::getSymbolName(
            srcPaths[0].empty()
                ? enclosing
                : srcPaths[0][0]->getParentOfType<FModuleLike>()));

        // Add the source path.
        for (auto inst : srcPaths[0]) {
          path += '.';
          path += getInnerRefTo(inst);
        }

        // Add the leaf value to the path.
        auto uloc = builder.getUnknownLoc();
        path += '.';
        if (auto blockArg = leafValue.dyn_cast<BlockArgument>()) {
          auto module = cast<FModuleOp>(blockArg.getOwner()->getParentOp());
          path += getInnerRefTo(module, blockArg.getArgNumber());
        } else {
          path += getInnerRefTo(leafValue.getDefiningOp());
        }

        // Assemble the verbatim op.
        builder.create<sv::VerbatimOp>(
            uloc,
            StringAttr::get(&getContext(), "assign " + path.getString() + ";"),
            ValueRange{}, ArrayAttr::get(&getContext(), path.getSymbols()));
        return true;
      })
      .Case<AugmentedVectorTypeAttr>([&](auto vector) {
        bool notFailed = true;
        auto elements = vector.getElements();
        for (size_t i = 0, e = elements.size(); i != e; ++i) {
          auto field = fromAttr(elements[i]);
          if (!field)
            return false;
          notFailed &=
              traverseField(field.getValue(), id,
                            path.snapshot().append("[" + Twine(i) + "]"));
        }
        return notFailed;
      })
      .Case<AugmentedBundleTypeAttr>([&](AugmentedBundleTypeAttr bundle) {
        bool anyFailed = true;
        for (auto element : bundle.getElements()) {
          auto field = fromAttr(element);
          if (!field)
            return false;
          auto name = element.cast<DictionaryAttr>().getAs<StringAttr>("name");
          if (!name)
            name = element.cast<DictionaryAttr>().getAs<StringAttr>("defName");
          anyFailed &=
              traverseField(field.getValue(), id,
                            path.snapshot().append("." + name.getValue()));
        }

        return anyFailed;
      })
      .Case<AugmentedStringTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedBooleanTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedIntegerTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedDoubleTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedLiteralTypeAttr>([&](auto a) { return false; })
      .Case<AugmentedDeletedTypeAttr>([&](auto a) { return false; })
      .Default([](auto a) { return true; });
}

Optional<TypeSum> GrandCentralPass::computeField(Attribute field,
                                                 IntegerAttr id,
                                                 StringAttr prefix,
                                                 VerbatimBuilder &path) {

  auto unsupported = [&](StringRef name, StringRef kind) {
    return VerbatimType({("// <unsupported " + kind + " type>").str(), false});
  };

  return TypeSwitch<Attribute, Optional<TypeSum>>(field)
      .Case<AugmentedGroundTypeAttr>(
          [&](AugmentedGroundTypeAttr ground) -> Optional<TypeSum> {
            // Traverse to generate mappings.
            traverseField(field, id, path);
            auto value = leafMap.lookup(ground.getID());
            auto tpe = value.getType().cast<FIRRTLType>();
            if (!tpe.isGround()) {
              value.getDefiningOp()->emitOpError()
                  << "cannot be added to interface with id '"
                  << id.getValue().getZExtValue()
                  << "' because it is not a ground type";
              return None;
            }
            return TypeSum(IntegerType::get(getOperation().getContext(),
                                            tpe.getBitWidthOrSentinel()));
          })
      .Case<AugmentedVectorTypeAttr>(
          [&](AugmentedVectorTypeAttr vector) -> Optional<TypeSum> {
            bool notFailed = true;
            auto elements = vector.getElements();
            auto firstElement = fromAttr(elements[0]);
            auto elementType =
                computeField(firstElement.getValue(), id, prefix,
                             path.snapshot().append("[" + Twine(0) + "]"));
            if (!elementType)
              return None;

            for (size_t i = 1, e = elements.size(); i != e; ++i) {
              auto subField = fromAttr(elements[i]);
              if (!subField)
                return None;
              notFailed &=
                  traverseField(subField.getValue(), id,
                                path.snapshot().append("[" + Twine(i) + "]"));
            }

            if (auto *tpe = std::get_if<Type>(&elementType.getValue()))
              return TypeSum(
                  hw::UnpackedArrayType::get(*tpe, elements.getValue().size()));
            auto str = std::get<VerbatimType>(elementType.getValue());
            str.dimensions.push_back(elements.getValue().size());
            return TypeSum(str);
          })
      .Case<AugmentedBundleTypeAttr>(
          [&](AugmentedBundleTypeAttr bundle) -> TypeSum {
            auto iface = traverseBundle(bundle, id, prefix, path);
            assert(iface && iface.getValue());
            return VerbatimType({getInterfaceName(prefix, bundle), true});
          })
      .Case<AugmentedStringTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "string");
      })
      .Case<AugmentedBooleanTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "boolean");
      })
      .Case<AugmentedIntegerTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "integer");
      })
      .Case<AugmentedDoubleTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "double");
      })
      .Case<AugmentedLiteralTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "literal");
      })
      .Case<AugmentedDeletedTypeAttr>([&](auto field) -> TypeSum {
        return unsupported(field.getName().getValue(), "deleted");
      });
}

/// Traverse an Annotation that is an AugmentedBundleType.  During
/// traversal, construct any discovered SystemVerilog interfaces.  If this
/// is the root interface, instantiate that interface in the parent. Recurse
/// into fields of the AugmentedBundleType to construct nested interfaces
/// and generate stringy-typed SystemVerilog hierarchical references to
/// drive the interface. Returns false on any failure and true on success.
Optional<sv::InterfaceOp>
GrandCentralPass::traverseBundle(AugmentedBundleTypeAttr bundle, IntegerAttr id,
                                 StringAttr prefix, VerbatimBuilder &path) {
  auto builder = OpBuilder::atBlockEnd(getOperation().getBody());
  sv::InterfaceOp iface;
  builder.setInsertionPointToEnd(getOperation().getBody());
  auto loc = getOperation().getLoc();
  auto iFaceName = getNamespace().newName(getInterfaceName(prefix, bundle));
  iface = builder.create<sv::InterfaceOp>(loc, iFaceName);
  if (maybeExtractInfo)
    iface->setAttr("output_file",
                   hw::OutputFileAttr::getFromDirectoryAndFilename(
                       &getContext(), getOutputDirectory().getValue(),
                       iFaceName + ".sv",
                       /*excludFromFileList=*/true));

  builder.setInsertionPointToEnd(cast<sv::InterfaceOp>(iface).getBody());

  for (auto element : bundle.getElements()) {
    auto field = fromAttr(element);
    if (!field)
      return None;

    auto name = element.cast<DictionaryAttr>().getAs<StringAttr>("name");
    // auto signalSym = hw::InnerRefAttr::get(iface.sym_nameAttr(), name);
    // TODO: The `append(name.getValue())` in the following should actually be
    // `append(signalSym)`, but this requires that `computeField` and the
    // functions it calls always return a type for which we can construct an
    // `InterfaceSignalOp`. Since nested interface instances are currently
    // busted (due to the interface being a symbol table), this doesn't work at
    // the moment. Passing a `name` works most of the time, but can be brittle
    // if the interface field requires renaming in the output (e.g. due to
    // naming conflicts).
    auto elementType =
        computeField(field.getValue(), id, prefix,
                     path.snapshot().append(".").append(name.getValue()));
    if (!elementType)
      return None;

    auto uloc = builder.getUnknownLoc();
    auto description =
        element.cast<DictionaryAttr>().getAs<StringAttr>("description");
    if (description) {
      auto descriptionOp = builder.create<sv::VerbatimOp>(
          uloc, ("// " + cleanupDescription(description.getValue())));

      // If we need to generate a YAML representation of this interface, then
      // add an attribute indicating that this `sv::VerbatimOp` is actually a
      // description.
      if (maybeHierarchyFileYAML)
        descriptionOp->setAttr("firrtl.grandcentral.yaml.type",
                               builder.getStringAttr("description"));
    }

    if (auto *str = std::get_if<VerbatimType>(&elementType.getValue())) {
      auto instanceOp =
          builder.create<sv::VerbatimOp>(uloc, str->toStr(name.getValue()));

      // If we need to generate a YAML representation of the interface, then add
      // attirbutes that describe what this `sv::VerbatimOp` is.
      if (maybeHierarchyFileYAML) {
        if (str->instantiation)
          instanceOp->setAttr("firrtl.grandcentral.yaml.type",
                              builder.getStringAttr("instance"));
        else
          instanceOp->setAttr("firrtl.grandcentral.yaml.type",
                              builder.getStringAttr("unsupported"));
        instanceOp->setAttr("firrtl.grandcentral.yaml.name", name);
        instanceOp->setAttr("firrtl.grandcentral.yaml.dimensions",
                            builder.getI32ArrayAttr(str->dimensions));
        instanceOp->setAttr(
            "firrtl.grandcentral.yaml.symbol",
            FlatSymbolRefAttr::get(builder.getContext(), str->str));
      }
      continue;
    }

    auto tpe = std::get<Type>(elementType.getValue());
    builder.create<sv::InterfaceSignalOp>(uloc, name.getValue(), tpe);
  }

  interfaceMap[FlatSymbolRefAttr::get(builder.getContext(), iFaceName)] = iface;
  return iface;
}

/// Return the module that is associated with this value.  Use the cached/lazily
/// constructed symbol table to make this fast.
FModuleLike GrandCentralPass::getEnclosingModule(Value value) {
  if (auto blockArg = value.dyn_cast<BlockArgument>())
    return cast<FModuleOp>(blockArg.getOwner()->getParentOp());

  auto *op = value.getDefiningOp();
  if (InstanceOp instance = dyn_cast<InstanceOp>(op))
    return getSymbolTable().lookup<FModuleOp>(
        instance.moduleNameAttr().getValue());

  return op->getParentOfType<FModuleOp>();
}

/// This method contains the business logic of this pass.
void GrandCentralPass::runOnOperation() {
  LLVM_DEBUG(llvm::dbgs() << "===- Running Grand Central Views/Interface Pass "
                             "-----------------------------===\n");

  CircuitOp circuitOp = getOperation();

  // Look at the circuit annotaitons to do two things:
  //
  // 1. Determine extraction information (directory and filename).
  // 2. Populate a worklist of all annotations that encode interfaces.
  //
  // Remove annotations encoding interfaces, but leave extraction information as
  // this may be needed by later passes.
  SmallVector<Annotation> worklist;
  bool removalError = false;
  AnnotationSet::removeAnnotations(circuitOp, [&](Annotation anno) {
    if (anno.isClass("sifive.enterprise.grandcentral.AugmentedBundleType")) {
      worklist.push_back(anno);
      return true;
    }
    if (anno.isClass(extractGrandCentralClass)) {
      if (maybeExtractInfo.hasValue()) {
        emitCircuitError("more than one 'ExtractGrandCentralAnnotation' was "
                         "found, but exactly one must be provided");
        removalError = true;
        return false;
      }

      auto directory = anno.getMember<StringAttr>("directory");
      auto filename = anno.getMember<StringAttr>("filename");
      if (!directory || !filename) {
        emitCircuitError()
            << "contained an invalid 'ExtractGrandCentralAnnotation' that does "
               "not contain 'directory' and 'filename' fields: "
            << anno.getDict();
        removalError = true;
        return false;
      }

      maybeExtractInfo = {directory, filename};
      // Do not delete this annotation.  Extraction info may be needed later.
      return false;
    }
    if (anno.isClass("sifive.enterprise.grandcentral."
                     "GrandCentralHierarchyFileAnnotation")) {
      if (maybeHierarchyFileYAML.hasValue()) {
        emitCircuitError("more than one 'GrandCentralHierarchyFileAnnotation' "
                         "was found, but zero or one may be provided");
        removalError = true;
        return false;
      }

      auto filename = anno.getMember<StringAttr>("filename");
      if (!filename) {
        emitCircuitError()
            << "contained an invalid 'GrandCentralHierarchyFileAnnotation' "
               "that does not contain 'directory' and 'filename' fields: "
            << anno.getDict();
        removalError = true;
        return false;
      }

      maybeHierarchyFileYAML = filename;
      return true;
    }
    if (anno.isClass(
            "sifive.enterprise.grandcentral.PrefixInterfacesAnnotation")) {
      if (!interfacePrefix.empty()) {
        emitCircuitError("more than one 'PrefixInterfacesAnnotation' was "
                         "found, but zero or one may be provided");
        removalError = true;
        return false;
      }

      auto prefix = anno.getMember<StringAttr>("prefix");
      if (!prefix) {
        emitCircuitError()
            << "contained an invalid 'PrefixInterfacesAnnotation' that does "
               "not contain a 'prefix' field: "
            << anno.getDict();
        removalError = true;
        return false;
      }

      interfacePrefix = prefix.getValue();
      return true;
    }
    return false;
  });

  if (removalError)
    return signalPassFailure();

  // Exit immediately if no annotations indicative of interfaces that need to be
  // built exist.
  if (worklist.empty())
    return markAllAnalysesPreserved();

  LLVM_DEBUG({
    llvm::dbgs() << "Extraction Info:\n";
    if (maybeExtractInfo)
      llvm::dbgs() << "  directory: " << maybeExtractInfo.getValue().directory
                   << "\n"
                   << "  filename: " << maybeExtractInfo.getValue().bindFilename
                   << "\n";
    else
      llvm::dbgs() << "  <none>\n";
    llvm::dbgs()
        << "Prefix Info (from PrefixInterfacesAnnotation):\n"
        << "  prefix: " << interfacePrefix << "\n"
        << "Hierarchy File Info (from GrandCentralHierarchyFileAnnotation):\n"
        << "  filename: ";
    if (maybeHierarchyFileYAML)
      llvm::dbgs() << maybeHierarchyFileYAML.getValue();
    else
      llvm::dbgs() << "<none>";
    llvm::dbgs() << "\n";
  });

  // Setup the builder to create ops _inside the FIRRTL circuit_.  This is
  // necessary because interfaces and interface instances are created.
  // Instances link to their definitions via symbols and we don't want to
  // break this.
  auto builder = OpBuilder::atBlockEnd(circuitOp.getBody());

  // Maybe get an "id" from an Annotation.  Generate error messages on the op if
  // no "id" exists.
  auto getID = [&](Operation *op,
                   Annotation annotation) -> Optional<IntegerAttr> {
    auto id = annotation.getMember<IntegerAttr>("id");
    if (!id) {
      op->emitOpError()
          << "contained a malformed "
             "'sifive.enterprise.grandcentral.AugmentedGroundType' annotation "
             "that did not contain an 'id' field";
      removalError = true;
      return None;
    }
    return Optional(id);
  };

  // Maybe return the lone instance of a module.  Generate errors on the op if
  // the module is not instantiated or is multiply instantiated.
  auto exactlyOneInstance = [&](FModuleOp op,
                                StringRef msg) -> Optional<InstanceOp> {
    auto uses = getSymbolTable().getSymbolUses(op, circuitOp);

    auto instances = llvm::make_filter_range(uses.getValue(), [&](auto u) {
      return (isa<InstanceOp>(u.getUser()));
    });

    if (instances.empty()) {
      op->emitOpError() << "is marked as a GrandCentral '" << msg
                        << "', but is never instantiated";
      return None;
    }

    if (llvm::hasSingleElement(instances))
      return cast<InstanceOp>((*(instances.begin())).getUser());

    auto diag = op->emitOpError() << "is marked as a GrandCentral '" << msg
                                  << "', but it is instantiated more than once";
    for (auto instance : instances)
      diag.attachNote(instance.getUser()->getLoc())
          << "parent is instantiated here";
    return None;
  };

  /// Walk the circuit and extract all information related to scattered
  /// Grand Central annotations.  This is used to populate: (1) the
  /// companionIDMap, (2) the parentIDMap, and (3) the leafMap.
  /// Annotations are removed as they are discovered and if they are not
  /// malformed.
  removalError = false;
  auto trueAttr = builder.getBoolAttr(true);
  circuitOp.walk([&](Operation *op) {
    TypeSwitch<Operation *>(op)
        .Case<RegOp, RegResetOp, WireOp, NodeOp>([&](auto op) {
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            auto maybeID = getID(op, annotation);
            if (!maybeID)
              return false;
            leafMap[maybeID.getValue()] = op.getResult();
            return true;
          });
        })
        // TODO: Figure out what to do with this.
        .Case<InstanceOp>([&](auto op) {
          AnnotationSet::removePortAnnotations(op, [&](unsigned i,
                                                       Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            op.emitOpError()
                << "is marked as an interface element, but this should be "
                   "impossible due to how the Chisel Grand Central API works";
            removalError = true;
            return false;
          });
        })
        .Case<MemOp>([&](auto op) {
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.AugmentedGroundType"))
              return false;
            op.emitOpError()
                << "is marked as an interface element, but this does not make "
                   "sense (is there a scattering bug or do you have a "
                   "malformed hand-crafted MLIR circuit?)";
            removalError = true;
            return false;
          });
          AnnotationSet::removePortAnnotations(
              op, [&](unsigned i, Annotation annotation) {
                if (!annotation.isClass(
                        "sifive.enterprise.grandcentral.AugmentedGroundType"))
                  return false;
                op.emitOpError()
                    << "has port '" << i
                    << "' marked as an interface element, but this does not "
                       "make sense (is there a scattering bug or do you have a "
                       "malformed hand-crafted MLIR circuit?)";
                removalError = true;
                return false;
              });
        })
        .Case<FModuleOp>([&](FModuleOp op) {
          // Handle annotations on the ports.
          AnnotationSet::removePortAnnotations(
              op, [&](unsigned i, Annotation annotation) {
                if (!annotation.isClass(
                        "sifive.enterprise.grandcentral.AugmentedGroundType"))
                  return false;
                auto maybeID = getID(op, annotation);
                if (!maybeID)
                  return false;
                leafMap[maybeID.getValue()] = op.getArgument(i);

                return true;
              });

          // Handle annotations on the module.
          AnnotationSet::removeAnnotations(op, [&](Annotation annotation) {
            if (!annotation.isClass(
                    "sifive.enterprise.grandcentral.ViewAnnotation"))
              return false;
            auto tpe = annotation.getMember<StringAttr>("type");
            auto name = annotation.getMember<StringAttr>("name");
            auto id = annotation.getMember<IntegerAttr>("id");
            if (!tpe) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain a 'type' field with a 'StringAttr' value";
              goto FModuleOp_error;
            }
            if (!id) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain an 'id' field with an 'IntegerAttr' value";
              goto FModuleOp_error;
            }
            if (!name) {
              op.emitOpError()
                  << "has a malformed "
                     "'sifive.enterprise.grandcentral.ViewAnnotation' that did "
                     "not contain a 'name' field with a 'StringAttr' value";
              goto FModuleOp_error;
            }

            // If this is a companion, then:
            //   1. Insert it into the companion map
            //   2. Create a new mapping module.
            //   3. Instatiate the mapping module in the companion.
            //   4. Check that the companion is instantated exactly once.
            //   5. Set attributes on that lone instance so it will become a
            //      bind if extraction information was provided.
            if (tpe.getValue() == "companion") {
              builder.setInsertionPointToEnd(circuitOp.getBody());

              // Create the mapping module.
              auto mappingName =
                  getNamespace().newName(name.getValue() + "_mapping");
              auto mapping = builder.create<FModuleOp>(
                  circuitOp.getLoc(), builder.getStringAttr(mappingName),
                  ArrayRef<PortInfo>());
              if (maybeExtractInfo)
                mapping->setAttr(
                    "output_file",
                    hw::OutputFileAttr::getFromDirectoryAndFilename(
                        &getContext(), getOutputDirectory().getValue(),
                        mapping.getName() + ".sv",
                        /*excludeFromFilelist=*/true));
              companionIDMap[id] = {name.getValue(), op, mapping};

              // Instantiate the mapping module inside the companion.
              builder.setInsertionPointToEnd(op.getBody());
              builder.create<InstanceOp>(circuitOp.getLoc(), mapping,
                                         mapping.getName());

              // Assert that the companion is instantiated once and only once.
              auto instance = exactlyOneInstance(op, "companion");
              if (!instance)
                return false;

              // If no extraction info was provided, exit.  Otherwise, setup the
              // lone instance of the companion to be lowered as a bind.
              if (!maybeExtractInfo)
                return true;

              instance.getValue()->setAttr("lowerToBind", trueAttr);
              instance.getValue()->setAttr(
                  "output_file",
                  hw::OutputFileAttr::getFromFilename(
                      &getContext(),
                      maybeExtractInfo.getValue().bindFilename.getValue(),
                      /*excludeFromFileList=*/true));
              op->setAttr("output_file",
                          hw::OutputFileAttr::getFromDirectoryAndFilename(
                              &getContext(),
                              maybeExtractInfo.getValue().directory.getValue(),
                              op.getName() + ".sv",
                              /*excludeFromFileList=*/true));
              return true;
            }

            // Insert the parent into the parent map, asserting that the parent
            // is instantiated exatly once.
            if (tpe.getValue() == "parent") {
              // Assert that the parent is instantiated once and only once.
              // Allow for this to be the main module in the circuit.
              Optional<InstanceOp> instance;
              if (op != circuitOp.getMainModule()) {
                instance = exactlyOneInstance(op, "parent");
                if (!instance && circuitOp.getMainModule() != op)
                  return false;
              }

              parentIDMap[id] = {instance, cast<FModuleOp>(op)};
              return true;
            }

            op.emitOpError()
                << "has a 'sifive.enterprise.grandcentral.ViewAnnotation' with "
                   "an unknown or malformed 'type' field in annotation: "
                << annotation.getDict();

          FModuleOp_error:
            removalError = true;
            return false;
          });
        });
  });

  if (removalError)
    return signalPassFailure();

  // Check that a parent exists for every companion.
  for (auto a : companionIDMap) {
    if (parentIDMap.count(a.first) == 0) {
      emitCircuitError()
          << "contains a 'companion' with id '"
          << a.first.cast<IntegerAttr>().getValue().getZExtValue()
          << "', but does not contain a GrandCentral 'parent' with the same id";
      return signalPassFailure();
    }
  }

  // Check that a companion exists for every parent.
  for (auto a : parentIDMap) {
    if (companionIDMap.count(a.first) == 0) {
      emitCircuitError()
          << "contains a 'parent' with id '"
          << a.first.cast<IntegerAttr>().getValue().getZExtValue()
          << "', but does not contain a GrandCentral 'companion' with the same "
             "id";
      return signalPassFailure();
    }
  }

  LLVM_DEBUG({
    // Print out the companion map, parent map, and all leaf values that
    // were discovered.  Sort these by their keys before printing to make
    // this easier to read.
    SmallVector<IntegerAttr> ids;
    auto sort = [&ids]() {
      llvm::sort(ids, [](IntegerAttr a, IntegerAttr b) {
        return a.getValue().getZExtValue() < b.getValue().getZExtValue();
      });
    };
    for (auto tuple : companionIDMap)
      ids.push_back(tuple.first.cast<IntegerAttr>());
    sort();
    llvm::dbgs() << "companionIDMap:\n";
    for (auto id : ids) {
      auto value = companionIDMap.lookup(id);
      llvm::dbgs() << "  - " << id.getValue() << ": "
                   << value.companion.getName() << " -> " << value.name << "\n";
    }
    llvm::dbgs() << "parentIDMap:\n";
    for (auto id : ids) {
      auto value = parentIDMap.lookup(id);
      StringRef name;
      if (value.first)
        name = value.first.getValue().name();
      else
        name = value.second.getName();
      llvm::dbgs() << "  - " << id.getValue() << ": " << name << ":"
                   << value.second.getName() << "\n";
    }
    ids.clear();
    for (auto tuple : leafMap)
      ids.push_back(tuple.first.cast<IntegerAttr>());
    sort();
    llvm::dbgs() << "leafMap:\n";
    for (auto id : ids) {
      auto value = leafMap.lookup(id);
      if (auto blockArg = value.dyn_cast<BlockArgument>()) {
        FModuleOp module = cast<FModuleOp>(blockArg.getOwner()->getParentOp());
        llvm::dbgs() << "  - " << id.getValue() << ": "
                     << module.getName() + ">" +
                            module.getPortName(blockArg.getArgNumber())
                     << "\n";
      } else
        llvm::dbgs() << "  - " << id.getValue() << ": "
                     << value.getDefiningOp()
                            ->getAttr("name")
                            .cast<StringAttr>()
                            .getValue()
                     << "\n";
    }
  });

  /// TODO: Handle this differently to allow construction of an optionsl
  auto instancePathCache = InstancePathCache(getAnalysis<InstanceGraph>());
  instancePaths = &instancePathCache;

  // Now, iterate over the worklist of interface-encoding annotations to create
  // the interface and all its sub-interfaces (interfaces that it instantiates),
  // instantiate the top-level interface, and generate a "mappings file" that
  // will use XMRs to drive the interface.  If extraction info is available,
  // then the top-level instantiate interface will be marked for extraction via
  // a SystemVerilog bind.
  SmallVector<sv::InterfaceOp, 2> interfaceVec;
  for (auto anno : worklist) {
    auto bundle = AugmentedBundleTypeAttr::get(&getContext(), anno.getDict());

    // The top-level AugmentedBundleType must have a global ID field so that
    // this can be linked to the parent and companion.
    if (!bundle.isRoot()) {
      emitCircuitError() << "missing 'id' in root-level BundleType: "
                         << anno.getDict() << "\n";
      removalError = true;
      continue;
    }

    // Error if a matching parent or companion do not exist.
    if (parentIDMap.count(bundle.getID()) == 0) {
      emitCircuitError() << "no parent found with 'id' value '"
                         << bundle.getID().getValue().getZExtValue() << "'\n";
      removalError = true;
      continue;
    }
    if (companionIDMap.count(bundle.getID()) == 0) {
      emitCircuitError() << "no companion found with 'id' value '"
                         << bundle.getID().getValue().getZExtValue() << "'\n";
      removalError = true;
      continue;
    }

    // Decide on a symbol name to use for the interface instance. This is needed
    // in `traverseBundle` as a placeholder for the connect operations.
    auto parentModule = parentIDMap.lookup(bundle.getID()).second;
    auto symbolName = getNamespace().newName(
        "__" + companionIDMap.lookup(bundle.getID()).name + "_" +
        getInterfaceName(bundle.getPrefix(), bundle) + "__");

    // Recursively walk the AugmentedBundleType to generate interfaces and XMRs.
    // Error out if this returns None (indicating that the annotation annotation
    // is malformed in some way).  A good error message is generated inside
    // `traverseBundle` or the functions it calls.
    VerbatimBuilder::Base verbatimData;
    VerbatimBuilder verbatim(verbatimData);
    verbatim +=
        hw::InnerRefAttr::get(SymbolTable::getSymbolName(parentModule),
                              StringAttr::get(&getContext(), symbolName));
    auto iface =
        traverseBundle(bundle, bundle.getID(), bundle.getPrefix(), verbatim);
    if (!iface) {
      removalError = true;
      continue;
    }

    interfaceVec.push_back(iface.getValue());

    // Instantiate the interface inside the parent.
    builder.setInsertionPointToEnd(parentModule.getBody());
    auto instance = builder.create<sv::InterfaceInstanceOp>(
        getOperation().getLoc(), iface.getValue().getInterfaceType(),
        companionIDMap.lookup(bundle.getID()).name,
        builder.getStringAttr(symbolName));

    // If no extraction information was present, then just leave the interface
    // instantiated in the parent.  Otherwise, make it a bind.
    if (!maybeExtractInfo)
      continue;

    instance->setAttr("doNotPrint", trueAttr);
    builder.setInsertionPointToStart(
        instance->getParentOfType<ModuleOp>().getBody());
    auto bind = builder.create<sv::BindInterfaceOp>(
        getOperation().getLoc(),
        SymbolRefAttr::get(builder.getContext(),
                           instance.sym_name().getValue()));
    bind->setAttr("output_file",
                  hw::OutputFileAttr::getFromFilename(
                      &getContext(),
                      maybeExtractInfo.getValue().bindFilename.getValue(),
                      /*excludeFromFileList=*/true));
  }

  // If a `GrandCentralHierarchyFileAnnotation` was passed in, generate a YAML
  // representation of the interfaces that we produced with the filename that
  // that annotation provided.
  if (maybeHierarchyFileYAML) {
    std::string yamlString;
    llvm::raw_string_ostream stream(yamlString);
    ::yaml::Context yamlContext({interfaceMap});
    llvm::yaml::Output yout(stream);
    yamlize(yout, interfaceVec, true, yamlContext);

    builder.create<sv::VerbatimOp>(builder.getUnknownLoc(), yamlString)
        ->setAttr("output_file",
                  hw::OutputFileAttr::getFromFilename(
                      &getContext(),
                      maybeHierarchyFileYAML.getValue().getValue(),
                      /*excludFromFileList=*/true));
    LLVM_DEBUG({ llvm::dbgs() << "Generated YAML:" << yamlString << "\n"; });
  }

  // Signal pass failure if any errors were found while examining circuit
  // annotations.
  if (removalError)
    return signalPassFailure();
}

StringAttr GrandCentralPass::getOrAddInnerSym(Operation *op) {
  auto attr = op->getAttrOfType<StringAttr>("inner_sym");
  if (attr)
    return attr;
  auto module = op->getParentOfType<FModuleOp>();
  StringRef nameHint = "gct_sym";
  if (auto attr = op->getAttrOfType<StringAttr>("name"))
    nameHint = attr.getValue();
  auto name = getModuleNamespace(module).newName(nameHint);
  attr = StringAttr::get(op->getContext(), name);
  op->setAttr("inner_sym", attr);
  return attr;
}

StringAttr GrandCentralPass::getOrAddInnerSym(FModuleLike module,
                                              size_t portIdx) {
  auto attr = module.getPortSymbolAttr(portIdx);
  if (attr && !attr.getValue().empty())
    return attr;
  StringRef nameHint = "gct_sym";
  if (auto attr = module.getPortNameAttr(portIdx))
    nameHint = attr.getValue();
  auto name = getModuleNamespace(module).newName(nameHint);
  attr = StringAttr::get(module.getContext(), name);
  module.setPortSymbolAttr(portIdx, attr);
  return attr;
}

hw::InnerRefAttr GrandCentralPass::getInnerRefTo(Operation *op) {
  return hw::InnerRefAttr::get(
      SymbolTable::getSymbolName(op->getParentOfType<FModuleOp>()),
      getOrAddInnerSym(op));
}

hw::InnerRefAttr GrandCentralPass::getInnerRefTo(FModuleLike module,
                                                 size_t portIdx) {
  return hw::InnerRefAttr::get(SymbolTable::getSymbolName(module),
                               getOrAddInnerSym(module, portIdx));
}

//===----------------------------------------------------------------------===//
// Pass Creation
//===----------------------------------------------------------------------===//

std::unique_ptr<mlir::Pass> circt::firrtl::createGrandCentralPass() {
  return std::make_unique<GrandCentralPass>();
}

//===----------------------------------------------------------------------===//
// Annotation Handling
//===----------------------------------------------------------------------===//

/// Implements the same behavior as DictionaryAttr::getAs<A> to return the value
/// of a specific type associated with a key in a dictionary.  However, this is
/// specialized to print a useful error message, specific to custom annotation
/// process, on failure.
template <typename A>
static A tryGetAs(DictionaryAttr &dict, const Attribute &root, StringRef key,
                  Location loc, Twine className, Twine path = Twine()) {
  // Check that the key exists.
  auto value = dict.get(key);
  if (!value) {
    SmallString<128> msg;
    if (path.isTriviallyEmpty())
      msg = ("Annotation '" + className + "' did not contain required key '" +
             key + "'.")
                .str();
    else
      msg = ("Annotation '" + className + "' with path '" + path +
             "' did not contain required key '" + key + "'.")
                .str();
    mlir::emitError(loc, msg).attachNote()
        << "The full Annotation is reproduced here: " << root << "\n";
    return nullptr;
  }
  // Check that the value has the correct type.
  auto valueA = value.dyn_cast_or_null<A>();
  if (!valueA) {
    SmallString<128> msg;
    if (path.isTriviallyEmpty())
      msg = ("Annotation '" + className +
             "' did not contain the correct type for key '" + key + "'.")
                .str();
    else
      msg = ("Annotation '" + className + "' with path '" + path +
             "' did not contain the correct type for key '" + key + "'.")
                .str();
    mlir::emitError(loc, msg).attachNote()
        << "The full Annotation is reproduced here: " << root << "\n";
    return nullptr;
  }
  return valueA;
}

/// Return an input \p target string in canonical form.  This converts a Legacy
/// Annotation (e.g., A.B.C) into a modern annotation (e.g., ~A|B>C).  Trailing
/// subfield/subindex references are preserved.
static SmallString<32> canonicalizeTarget(StringRef target) {

  // If this is a normal Target (not a Named), erase that field in the JSON
  // object and return that Target.
  if (target[0] == '~')
    return target;

  // This is a legacy target using the firrtl.annotations.Named type.  This
  // can be trivially canonicalized to a non-legacy target, so we do it with
  // the following three mappings:
  //   1. CircuitName => CircuitTarget, e.g., A -> ~A
  //   2. ModuleName => ModuleTarget, e.g., A.B -> ~A|B
  //   3. ComponentName => ReferenceTarget, e.g., A.B.C -> ~A|B>C
  SmallString<32> newTarget("~");
  unsigned tokenIdx = 0;
  for (auto a : target) {
    if (a == '.') {
      switch (tokenIdx) {
      case 0:
        newTarget += '|';
        break;
      case 1:
        newTarget += '>';
        break;
      default:
        newTarget += '\'';
        break;
      }
      ++tokenIdx;
    } else
      newTarget += a;
  }
  return newTarget;
}

static StringAttr canonicalizeTarget(StringAttr target) {

  // If this is a normal Target (not a Named), erase that field in the JSON
  // object and return that Target.
  if (target.getValue()[0] == '~')
    return target;

  return StringAttr::get(target.getContext(),
                         canonicalizeTarget(target.getValue()));
}

//===----------------------------------------------------------------------===//
// Specific annotation implementations
//===----------------------------------------------------------------------===//

LogicalResult circt::firrtl::applyGCMemTap(AnnoPathValue target, DictionaryAttr anno,
                            AnnoApplyState state) {
  auto context = state.circuit.getContext();
  auto clazz = "sifive.enterprise.grandcentral.MemTapAnnotation";
  auto classAttr = StringAttr::get(context, clazz);
  auto loc = state.circuit.getLoc();
  auto id = state.newID();

  NamedAttrList attrs;
  attrs.append("class", classAttr);
  attrs.append("id", id);
  auto sourceAttr = tryGetAs<StringAttr>(anno, anno, "source", loc, clazz);
  if (!sourceAttr)
    return failure();
  state.applyAnno(canonicalizeTarget(sourceAttr.getValue()) , attrs);

  auto tapsAttr =
      tryGetAs<ArrayAttr>(anno, anno, "taps", state.circuit.getLoc(), clazz);
  if (!tapsAttr)
    return failure();
  for (size_t i = 0, e = tapsAttr.size(); i != e; ++i) {
    auto tap = tapsAttr[i].dyn_cast_or_null<StringAttr>();
    if (!tap) {
      mlir::emitError(
          loc,
          "Annotation '" + Twine(clazz) + "' with path '.taps[" + Twine(i) +
              "]' contained an unexpected type (expected a string).")
              .attachNote()
          << "The full Annotation is reprodcued here: " << anno << "\n";
      return failure();
    }
    NamedAttrList foo;
    foo.append("class", classAttr);
    foo.append("id", id);
    foo.append("word", IntegerAttr::get(IntegerType::get(context, 64), i));
    state.applyAnno(canonicalizeTarget(tap), foo);
  }
  return success();
}


// Describes tap points into the design.  This has the following structure:
//   blackBox: ModuleTarget
//   keys: Seq[DataTapKey]
// DataTapKey has multiple implementations:
//   - ReferenceDataTapKey: (tapping a point which exists in the FIRRTL)
//       portName: ReferenceTarget
//       source: ReferenceTarget
//   - DataTapModuleSignalKey: (tapping a point, by name, in a blackbox)
//       portName: ReferenceTarget
//       module: IsModule
//       internalPath: String
//   - DeletedDataTapKey: (not implemented here)
//       portName: ReferenceTarget
//   - LiteralDataTapKey: (not implemented here)
//       portName: ReferenceTarget
//       literal: Literal
// A Literal is a FIRRTL IR literal serialized to a string.  For now, just
// store the string.
// TODO: Parse the literal string into a UInt or SInt literal.
LogicalResult circt::firrtl::applyGCDataTap(AnnoPathValue target, DictionaryAttr anno,
                             AnnoApplyState state) {
  auto context = state.circuit.getContext();
  auto clazz = "sifive.enterprise.grandcentral.DataTapsAnnotation";
  auto classAttr = StringAttr::get(context, clazz);
  auto loc = state.circuit.getLoc();
  auto id = state.newID();

  NamedAttrList attrs;
  attrs.append("class", classAttr);
  auto blackBoxAttr = tryGetAs<StringAttr>(anno, anno, "blackBox", loc, clazz);
  if (!blackBoxAttr)
    return failure();
  auto ctarget = canonicalizeTarget(blackBoxAttr.getValue());
  state.applyAnno(ctarget, attrs);
  state.setDontTouch(ctarget);

  // Process all the taps.
  auto keyAttr = tryGetAs<ArrayAttr>(anno, anno, "keys", loc, clazz);
  if (!keyAttr)
    return failure();
  for (size_t i = 0, e = keyAttr.size(); i != e; ++i) {
    auto b = keyAttr[i];
    auto path = ("keys[" + Twine(i) + "]").str();
    auto bDict = b.cast<DictionaryAttr>();
    auto classAttr =
        tryGetAs<StringAttr>(bDict, anno, "class", loc, clazz, path);
    if (!classAttr)
      return failure();

    // The "portName" field is common across all sub-types of DataTapKey.
    NamedAttrList port;
    auto portNameAttr =
        tryGetAs<StringAttr>(bDict, anno, "portName", loc, clazz, path);
    if (!portNameAttr)
      return failure();
    auto maybePortTarget = canonicalizeTarget(portNameAttr.getValue());
    auto portPair =
        splitAndAppendTarget(port, maybePortTarget, context);
    port.append("class", classAttr);
    port.append("id", id);

    if (classAttr.getValue() ==
        "sifive.enterprise.grandcentral.ReferenceDataTapKey") {
      NamedAttrList source;
      auto portID = state.newID();
      source.append("class", bDict.get("class"));
      source.append("id", id);
      source.append("portID", portID);
      source.append("type", StringAttr::get(context, "source"));
      auto sourceAttr =
          tryGetAs<StringAttr>(bDict, anno, "source", loc, clazz, path);
      if (!sourceAttr)
        return failure();
      auto maybeSourceTarget = canonicalizeTarget(sourceAttr.getValue());
      state.applyAnno(maybeSourceTarget, source);
      state.setDontTouch(maybeSourceTarget);

      // Port Annotations generation.
      port.append("portID", portID);
      port.append("type", StringAttr::get(context, "portName"));
      state.applyAnno(portPair.first, port);
      state.setDontTouch(portPair.first);
      continue;
    }

    if (classAttr.getValue() ==
        "sifive.enterprise.grandcentral.DataTapModuleSignalKey") {
      NamedAttrList module;
      auto portID = state.newID();
      module.append("class", classAttr);
      module.append("id", id);
      auto internalPathAttr =
          tryGetAs<StringAttr>(bDict, anno, "internalPath", loc, clazz, path);
      auto moduleAttr =
          tryGetAs<StringAttr>(bDict, anno, "module", loc, clazz, path);
      if (!internalPathAttr || !moduleAttr)
        return failure();
      module.append("internalPath", internalPathAttr);
      module.append("portID", portID);
      auto moduleTarget = canonicalizeTarget(moduleAttr.getValue());
      state.applyAnno(moduleTarget, module);
      state.setDontTouch(moduleTarget);

      // Port Annotations generation.
      port.append("portID", portID);
      state.applyAnno(portPair.first, port);
      continue;
    }

    if (classAttr.getValue() ==
        "sifive.enterprise.grandcentral.DeletedDataTapKey") {
      // Port Annotations generation.
      state.applyAnno(portPair.first, port);
      continue;
    }

    if (classAttr.getValue() ==
        "sifive.enterprise.grandcentral.LiteralDataTapKey") {
      NamedAttrList literal;
      literal.append("class", classAttr);
      auto literalAttr =
          tryGetAs<StringAttr>(bDict, anno, "literal", loc, clazz, path);
      if (!literalAttr)
        return failure();
      literal.append("literal", literalAttr);

      // Port Annotaiton generation.
      state.applyAnno(portPair.first, literal);
      continue;
    }

    mlir::emitError(
        loc, "Annotation '" + Twine(clazz) + "' with path '" + path + ".class" +
                 +"' contained an unknown/unimplemented DataTapKey class '" +
                 classAttr.getValue() + "'.")
            .attachNote()
        << "The full Annotation is reprodcued here: " << anno << "\n";
    return failure();
    }
  }

// Scatter signal driver annotations to the sources *and* the targets of the
// drives.
LogicalResult circt::firrtl::applyGCSigDriver(AnnoPathValue target, DictionaryAttr anno,
                               AnnoApplyState state) {
  auto context = state.circuit.getContext();
  auto clazz = "sifive.enterprise.grandcentral.SignalDriverAnnotation";
  auto classAttr = StringAttr::get(context, clazz);
  auto loc = state.circuit.getLoc();
  auto id = state.newID();

  // Rework the circuit-level annotation to no longer include the
  // information we are scattering away anyway.
  NamedAttrList fields;
  auto annotationsAttr = tryGetAs<ArrayAttr>(anno, anno, "annotations",
                                             state.circuit.getLoc(), clazz);
  auto circuitAttr = tryGetAs<StringAttr>(anno, anno, "circuit",
                                          state.circuit.getLoc(), clazz);
  auto circuitPackageAttr = tryGetAs<StringAttr>(anno, anno, "circuitPackage",
                                                 state.circuit.getLoc(), clazz);
  if (!annotationsAttr || !circuitAttr || !circuitPackageAttr)
    return failure();
  // TODO  fields.append("class", classAttr);
  fields.append("id", id);
  fields.append("annotations", annotationsAttr);
  fields.append("circuit", circuitAttr);
  fields.append("circuitPackage", circuitPackageAttr);
  state.applyAnno(canonicalizeTarget("~"), fields);

  // A callback that will scatter every source and sink target pair to the
  // corresponding two ends of the connection.
  llvm::StringSet annotatedModules;
  auto handleTarget = [&](Attribute attr, unsigned i, bool isSource) {
    auto targetId = state.newID();
    DictionaryAttr targetDict = attr.dyn_cast<DictionaryAttr>();
    if (!targetDict) {
      mlir::emitError(state.circuit.getLoc(),
                      "SignalDriverAnnotation source and sink target "
                      "entries must be dictionaries")
              .attachNote()
          << "annotation:" << anno << "\n";
      return false;
    }

    // Dig up the two sides of the link.
    auto path = (Twine(clazz) + "." + (isSource ? "source" : "sink") +
                 "Targets[" + Twine(i) + "]")
                    .str();
    auto remoteAttr = tryGetAs<StringAttr>(targetDict, anno, "_1",
                                           state.circuit.getLoc(), path);
    auto localAttr = tryGetAs<StringAttr>(targetDict, anno, "_2",
                                          state.circuit.getLoc(), path);
    if (!localAttr || !remoteAttr)
      return false;

    // Build the two annotations.
    for (auto pair : std::array{std::make_pair(localAttr, true),
                                std::make_pair(remoteAttr, false)}) {
      auto canonTarget = canonicalizeTarget(pair.first.getValue());

      // HACK: Ignore the side of the connection that targets the *other*
      // circuit. We do this by checking whether the canonicalized target
      // begins with `~CircuitName|`. If it doesn't, we skip.
      // TODO: Once we properly support multiple circuits, this can go and
      // the annotation can scatter properly.
      StringRef prefix(canonTarget);
      if (!(prefix.consume_front("~") &&
            prefix.consume_front(state.circuit.name()) &&
            prefix.consume_front("|"))) {
        return true;
      }

      // Assemble the annotation on this side of the connection.
      NamedAttrList fields;
      // TODO      fields.append("class", classAttr);
      fields.append("id", id);
      fields.append("targetId", targetId);
      fields.append("peer", pair.second ? remoteAttr : localAttr);
      fields.append("side", StringAttr::get(state.circuit.getContext(),
                                            pair.second ? "local" : "remote"));
      fields.append("dir", StringAttr::get(state.circuit.getContext(),
                                           isSource ? "source" : "sink"));

      state.applyAnno(canonTarget, fields);

      // Add a don't touch annotation to whatever this annotation targets.
      state.setDontTouch(canonTarget);

      // Keep track of the enclosing module.
//TODO
      // annotatedModules.insert(
      //     (StringRef(std::get<0>(NLATargets.back())).split("|").first + "|" +
      //      std::get<1>(NLATargets.back()))
      //         .str());
    }

    return true;
  };

  // Handle the source and sink targets.
  auto sourcesAttr =
      tryGetAs<ArrayAttr>(anno, anno, "sourceTargets", loc, clazz);
  auto sinksAttr = tryGetAs<ArrayAttr>(anno, anno, "sinkTargets", loc, clazz);
  if (!sourcesAttr || !sinksAttr)
    return failure();
  unsigned i = 0;
  for (auto attr : sourcesAttr)
    if (!handleTarget(attr, i++, true))
      return failure();
  i = 0;
  for (auto attr : sinksAttr)
    if (!handleTarget(attr, i++, false))
      return failure();

  // Indicate which modules have embedded `SignalDriverAnnotation`s.
  for (auto &module : annotatedModules) {
    NamedAttrList fields;
    fields.append("class", classAttr);
    fields.append("id", id);
    state.applyAnno(module.getKey(), fields);
  }

  return success();
}

#if 0 // TODO

LogicalResult applyGCView(AnnoPathValue target, DictionaryAttr anno,
                          AnnoApplyState state) {
auto context = state.circuit.getContext();
  //        if (clazz == "sifive.enterprise.grandcentral.GrandCentralView$"
  //                   "SerializedViewAnnotation" ||
  //        clazz == "sifive.enterprise.grandcentral.ViewAnnotation") {
  auto viewAnnotationClass =
      StringAttr::get(context, "sifive.enterprise.grandcentral.ViewAnnotation");
  auto id = state.newID();
  NamedAttrList companionAttrs, parentAttrs;
  companionAttrs.append("class", viewAnnotationClass);
  companionAttrs.append("id", id);
  companionAttrs.append("type", StringAttr::get(context, "companion"));
  auto viewAttr = tryGetAs<DictionaryAttr>(anno, anno, "view",
                                           state.circuit.getLoc(), clazz);
  if (!viewAttr)
    return false;
  auto name =
      tryGetAs<StringAttr>(anno, anno, "name", state.circuit.getLoc(), clazz);
  if (!name)
    return false;
  companionAttrs.append("name", name);
  auto companionAttr = tryGetAs<StringAttr>(anno, anno, "companion",
                                            state.circuit.getLoc(), clazz);
  if (!companionAttr)
    return false;
  companionAttrs.append("target", companionAttr);
  newAnnotations.push_back(DictionaryAttr::get(context, companionAttrs));
  auto parentAttr =
      tryGetAs<StringAttr>(anno, anno, "parent", state.circuit.getLoc(), clazz);
  if (!parentAttr)
    return false;
  parentAttrs.append("class", viewAnnotationClass);
  parentAttrs.append("id", id);
  parentAttrs.append("name", name);
  parentAttrs.append("type", StringAttr::get(context, "parent"));
  parentAttrs.append("target", parentAttr);
  newAnnotations.push_back(DictionaryAttr::get(context, parentAttrs));
  auto prunedAttr = parseAugmentedType(
      context, viewAttr, anno, newAnnotations, companionAttr.getValue(), name,
      {}, id, {}, state.circuit.getLoc(), annotationID, clazz, "view");
  if (!prunedAttr)
    return false;

  newAnnotations.push_back(cloneWithNewField(prunedAttr.getValue(), "target",
                                             StringAttr::get(context, "~")));
  return true;
}

#endif

LogicalResult circt::firrtl::applyModRep(AnnoPathValue target, DictionaryAttr anno,
                          AnnoApplyState state) {
  auto context = state.circuit.getContext();
  auto clazz = "sifive.enterprise.grandcentral.ModuleReplacementAnnotation";
auto classAttr = StringAttr::get(context, clazz);
auto loc = state.circuit.getLoc();
auto id = state.newID();

NamedAttrList fields;
auto annotationsAttr = tryGetAs<ArrayAttr>(anno, anno, "annotations",
                                           state.circuit.getLoc(), clazz);
auto circuitAttr =
    tryGetAs<StringAttr>(anno, anno, "circuit", state.circuit.getLoc(), clazz);
auto circuitPackageAttr = tryGetAs<StringAttr>(anno, anno, "circuitPackage",
                                               state.circuit.getLoc(), clazz);
auto dontTouchesAttr = tryGetAs<ArrayAttr>(anno, anno, "dontTouches",
                                           state.circuit.getLoc(), clazz);
if (!annotationsAttr || !circuitAttr || !circuitPackageAttr || !dontTouchesAttr)
  return failure();
fields.append("class", classAttr);
fields.append("id", id);
fields.append("annotations", annotationsAttr);
fields.append("circuit", circuitAttr);
fields.append("circuitPackage", circuitPackageAttr);
state.applyAnno(canonicalizeTarget("~"), fields);

// Add a don't touches for each target in "dontTouches" list
for (auto dontTouch : dontTouchesAttr) {
  StringAttr targetString = dontTouch.dyn_cast<StringAttr>();
  if (!targetString) {
    mlir::emitError(state.circuit.getLoc(),
                    "ModuleReplacementAnnotation dontTouches "
                    "entries must be strings")
            .attachNote()
        << "annotation:" << anno << "\n";
    return failure();
  }
  auto canonTarget = canonicalizeTarget(targetString.getValue());

  // Add a don't touch annotation to whatever this annotation targets.
  state.setDontTouch(canonTarget);
  }

  auto targets =
      tryGetAs<ArrayAttr>(anno, anno, "targets", state.circuit.getLoc(), clazz);
  if (!targets)
    return failure();
  for (auto targetAttr : targets) {
    NamedAttrList fields;
    fields.append("id", id);
    StringAttr targetString = targetAttr.dyn_cast<StringAttr>();
    if (!targetString) {
      mlir::emitError(
          state.circuit.getLoc(),
          "ModuleReplacementAnnotation targets entries must be strings")
              .attachNote()
          << "annotation:" << anno << "\n";
      return failure();
    }
    auto canonTarget = canonicalizeTarget(targetString.getValue());
    state.applyAnno(canonTarget, fields);
  }
  return success();
}
