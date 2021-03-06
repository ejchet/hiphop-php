/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/base/execution_context.h"
#include "hphp/runtime/base/complex_types.h"
#include "hphp/runtime/base/zend_string.h"
#include "hphp/runtime/base/hphp_array.h"
#include "hphp/runtime/base/builtin_functions.h"
#include "hphp/runtime/ext/ext_closure.h"
#include "hphp/runtime/ext/ext_continuation.h"
#include "hphp/runtime/ext/ext_collections.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/repo.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

#include "hphp/runtime/base/zend_functions.h"
#include "hphp/runtime/ext/ext_string.h"

namespace HPHP {

TRACE_SET_MOD(runtime);

CompileStringFn g_hphp_compiler_parse;
BuildNativeFuncUnitFn g_hphp_build_native_func_unit;
BuildNativeClassUnitFn g_hphp_build_native_class_unit;

/**
 * print_string will decRef the string
 */
void print_string(StringData* s) {
  g_context->write(s->data(), s->size());
  TRACE(1, "t-x64 output(str): (%p) %43s\n", s->data(),
        Util::escapeStringForCPP(s->data(), s->size()).data());
  decRefStr(s);
}

void print_int(int64_t i) {
  char buf[256];
  snprintf(buf, 256, "%" PRId64, i);
  echo(buf);
  TRACE(1, "t-x64 output(int): %" PRId64 "\n", i);
}

void print_boolean(bool val) {
  if (val) {
    echo("1");
  }
}

HOT_FUNC_VM
ArrayData* new_array(int capacity) {
  ArrayData *a = ArrayData::Make(capacity);
  a->incRefCount();
  TRACE(2, "newArrayHelper: capacity %d\n", capacity);
  return a;
}

ArrayData* new_tuple(int n, const TypedValue* values) {
  auto a = ArrayData::Make(n, values);
  a->incRefCount();
  TRACE(2, "new_tuple: size %d\n", n);
  return a;
}

#define NEW_COLLECTION_HELPER(name) \
  ObjectData* \
  new##name##Helper(int nElms) { \
    ObjectData *obj = NEWOBJ(c_##name)(); \
    obj->incRefCount(); \
    if (nElms) { \
      collectionReserve(obj, nElms); \
    } \
    TRACE(2, "new" #name "Helper: capacity %d\n", nElms); \
    return obj; \
  }

NEW_COLLECTION_HELPER(Vector)
NEW_COLLECTION_HELPER(Map)
NEW_COLLECTION_HELPER(StableMap)
NEW_COLLECTION_HELPER(Set)

ObjectData* newPairHelper() {
  ObjectData *obj = NEWOBJ(c_Pair)();
  obj->incRefCount();
  TRACE(2, "newPairHelper: capacity 2\n");
  return obj;
}

#undef NEW_COLLECTION_HELPER

static inline void
tvPairToCString(DataType t, uint64_t v,
                const char** outStr,
                size_t* outSz,
                bool* outMustFree) {
  if (IS_STRING_TYPE(t)) {
    StringData *strd = (StringData*)v;
    *outStr = strd->data();
    *outSz = strd->size();
    *outMustFree = false;
    return;
  }
  Cell c;
  c.m_type = t;
  c.m_data.num = v;
  String s = tvAsVariant(&c).toString();
  *outStr = (const char*)malloc(s.size());
  TRACE(1, "t-x64: stringified: %s -> %s\n", s.data(), *outStr);
  memcpy((char*)*outStr, s.data(), s.size());
  *outSz = s.size();
  *outMustFree = true;
}

/**
 * concat_ss will decRef the values passed in as appropriate, and it will
 * incRef the output string
 */
StringData*
concat_ss(StringData* v1, StringData* v2) {
  if (v1->getCount() > 1) {
    StringData* ret = NEW(StringData)(v1, v2);
    ret->setRefCount(1);
    decRefStr(v2);
    // Because v1->getCount() is greater than 1, we know we will never
    // have to release the string here
    v1->decRefCount();
    return ret;
  } else {
    v1->append(v2->slice());
    decRefStr(v2);
    return v1;
  }
}

/**
 * concat_is will decRef the string passed in as appropriate, and it will
 * incRef the output string
 */
StringData*
concat_is(int64_t v1, StringData* v2) {
  int len1;
  char intbuf[21];
  char* intstart;
  // Convert the int to a string
  {
    int is_negative;
    intstart = conv_10(v1, &is_negative, intbuf + sizeof(intbuf), &len1);
  }
  StringSlice s1(intstart, len1);
  StringSlice s2 = v2->slice();
  StringData* ret = NEW(StringData)(s1, s2);
  ret->incRefCount();
  decRefStr(v2);
  return ret;
}

/**
 * concat_si will decRef the string passed in as appropriate, and it will
 * incRef the output string
 */
StringData*
concat_si(StringData* v1, int64_t v2) {
  int len2;
  char intbuf[21];
  char* intstart;
  // Convert the int to a string
  {
    int is_negative;
    intstart = conv_10(v2, &is_negative, intbuf + sizeof(intbuf), &len2);
  }
  StringSlice s1 = v1->slice();
  StringSlice s2(intstart, len2);
  StringData* ret = NEW(StringData)(s1, s2);
  ret->incRefCount();
  decRefStr(v1);
  return ret;
}

/**
 * concat will decRef the values passed in as appropriate, and it will
 * incRef the output string
 */
StringData*
concat_tv(DataType t1, uint64_t v1, DataType t2, uint64_t v2) {
  const char *s1, *s2;
  size_t s1len, s2len;
  bool free1, free2;
  tvPairToCString(t1, v1, &s1, &s1len, &free1);
  tvPairToCString(t2, v2, &s2, &s2len, &free2);
  StringSlice r1(s1, s1len);
  StringSlice r2(s2, s2len);
  StringData* retval = NEW(StringData)(r1, r2);
  retval->incRefCount();
  // If tvPairToCString allocated temporary buffers, free them now
  if (free1) free((void*)s1);
  if (free2) free((void*)s2);
  // decRef the parameters as appropriate
  tvRefcountedDecRefHelper(t2, v2);
  tvRefcountedDecRefHelper(t1, v1);

  return retval;
}

int64_t eq_null_str(StringData* v1) {
  int64_t retval = v1->empty();
  decRefStr(v1);
  return retval;
}

int64_t eq_bool_str(int64_t v1, StringData* v2) {
  // The truth table for v2->toBoolean() ? v1 : !v1
  //   looks like:
  //      \ v2:0 | v2:1
  // v1:0 |   1  |   0
  // v1:1 |   0  |   1
  //
  // which is nothing but nxor.
  int64_t v2i = int64_t(v2->toBoolean());
  assert(v2i == 0ll || v2i == 1ll);
  assert(v1  == 0ll || v1  == 1ll);
  int64_t retval = (v2i ^ v1) ^ 1;
  assert(retval == 0ll || retval == 1ll);
  decRefStr(v2);
  return retval;
}

int64_t eq_int_str(int64_t v1, StringData* v2) {
  int64_t lval; double dval;
  DataType ret = is_numeric_string(v2->data(), v2->size(), &lval, &dval, 1);
  decRefStr(v2);
  if (ret == KindOfInt64) {
    return v1 == lval;
  } else if (ret == KindOfDouble) {
    return (double)v1 == dval;
  } else {
    return v1 == 0;
  }
}

int64_t eq_str_str(StringData* v1, StringData* v2) {
  int64_t retval = v1->equal(v2);
  decRefStr(v2);
  decRefStr(v1);
  return retval;
}

int64_t same_str_str(StringData* v1, StringData* v2) {
  int64_t retval = v1 == v2 || v1->same(v2);
  decRefStr(v2);
  decRefStr(v1);
  return retval;
}

int64_t str0_to_bool(StringData* sd) {
  int64_t retval = sd->toBoolean();
  return retval;
}

int64_t str_to_bool(StringData* sd) {
  int64_t retval = str0_to_bool(sd);
  decRefStr(sd);
  return retval;
}

int64_t arr0_to_bool(ArrayData* ad) {
  return ad->size() != 0;
}

int64_t arr_to_bool(ArrayData* ad) {
  assert(Transl::Translator::Get()->stateIsDirty());
  int64_t retval = arr0_to_bool(ad);
  decRefArr(ad);
  return retval;
}

/**
 * tv_to_bool will decrement tv's refcount if tv is a refcounted type
 */
int64_t
tv_to_bool(TypedValue* tv) {
  using std::string;
  bool retval;
  if (IS_STRING_TYPE(tv->m_type)) {
    StringData* sd = tv->m_data.pstr;
    retval = bool(str0_to_bool(sd));
  } else if (tv->m_type == KindOfArray) {
    ArrayData* ad = tv->m_data.parr;
    retval = bool(arr0_to_bool(ad));
  } else {
    retval = tvAsCVarRef(tv).toBoolean();
  }
  TRACE(2, Trace::prettyNode("TvToBool", *tv) + string(" -> ") +
        string(retval ? "t" : "f") + string("\n"));
  tvRefcountedDecRef(tv);
  return int64_t(retval);
}

Unit* compile_file(const char* s, size_t sz, const MD5& md5,
                   const char* fname) {
  return g_hphp_compiler_parse(s, sz, md5, fname);
}

Unit* build_native_func_unit(const HhbcExtFuncInfo* builtinFuncs,
                             ssize_t numBuiltinFuncs) {
  return g_hphp_build_native_func_unit(builtinFuncs, numBuiltinFuncs);
}

Unit* build_native_class_unit(const HhbcExtClassInfo* builtinClasses,
                              ssize_t numBuiltinClasses) {
  return g_hphp_build_native_class_unit(builtinClasses, numBuiltinClasses);
}

Unit* compile_string(const char* s, size_t sz, const char* fname) {
  MD5 md5;
  int out_len;
  md5 = MD5(string_md5(s, sz, false, out_len));

  Unit* u = Repo::get().loadUnit(fname ? fname : "", md5);
  if (u != nullptr) {
    return u;
  }
  return g_hphp_compiler_parse(s, sz, md5, fname);
}

// Returned array has refcount zero! Caller must refcount.
HphpArray* pack_args_into_array(ActRec* ar, int nargs) {
  HphpArray* argArray = ArrayData::Make(nargs);
  for (int i = 0; i < nargs; ++i) {
    TypedValue* tv = (TypedValue*)(ar) - (i+1);
    argArray->HphpArray::appendWithRef(tvAsCVarRef(tv), false);
  }
  if (!ar->hasInvName()) {
    // If this is not a magic call, we're done
    return argArray;
  }
  // This is a magic call, so we need to shuffle the args
  HphpArray* magicArgs = ArrayData::Make(2);
  magicArgs->append(ar->getInvName(), false);
  magicArgs->append(argArray, false);
  return magicArgs;
}

HphpArray* get_static_locals(const ActRec* ar) {
  if (ar->m_func->isClosureBody()) {
    TypedValue* closureLoc = frame_local(ar, ar->m_func->numParams());
    assert(dynamic_cast<c_Closure*>(closureLoc->m_data.pobj));
    return static_cast<c_Closure*>(closureLoc->m_data.pobj)->getStaticLocals();
  } else if (ar->m_func->isGeneratorFromClosure()) {
    c_Continuation* cont = frame_continuation(ar);
    TypedValue* closureLoc = frame_local(ar, cont->m_origFunc->numParams());
    assert(dynamic_cast<c_Closure*>(closureLoc->m_data.pobj));
    return static_cast<c_Closure*>(closureLoc->m_data.pobj)->getStaticLocals();
  } else {
    return ar->m_func->getStaticLocals();
  }
}

void collection_setm_wk1_v0(ObjectData* obj, TypedValue* value) {
  assert(obj);
  collectionAppend(obj, value);
  // TODO Task #1970153: It would be great if we had a version of
  // collectionAppend() that didn't incRef the value so that we
  // wouldn't have to decRef it here
  tvRefcountedDecRef(value);
}

void collection_setm_ik1_v0(ObjectData* obj, int64_t key, TypedValue* value) {
  assert(obj);
  switch (obj->getCollectionType()) {
    case Collection::VectorType: {
      c_Vector* vec = static_cast<c_Vector*>(obj);
      vec->set(key, value);
      break;
    }
    case Collection::MapType: {
      c_Map* mp = static_cast<c_Map*>(obj);
      mp->set(key, value);
      break;
    }
    case Collection::StableMapType: {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->set(key, value);
      break;
    }
    case Collection::SetType: {
      Object e(SystemLib::AllocRuntimeExceptionObject(
        "Set does not support $c[$k] syntax"));
      throw e;
    }
    case Collection::PairType: {
      Object e(SystemLib::AllocRuntimeExceptionObject(
        "Cannot assign to an element of a Pair"));
      throw e;
    }
    default:
      assert(false);
  }
  tvRefcountedDecRef(value);
}

void collection_setm_sk1_v0(ObjectData* obj, StringData* key,
                            TypedValue* value) {
  switch (obj->getCollectionType()) {
    case Collection::VectorType: {
      Object e(SystemLib::AllocInvalidArgumentExceptionObject(
        "Only integer keys may be used with Vectors"));
      throw e;
    }
    case Collection::MapType: {
      c_Map* mp = static_cast<c_Map*>(obj);
      mp->set(key, value);
      break;
    }
    case Collection::StableMapType: {
      c_StableMap* smp = static_cast<c_StableMap*>(obj);
      smp->set(key, value);
      break;
    }
    case Collection::SetType: {
      Object e(SystemLib::AllocRuntimeExceptionObject(
        "Set does not support $c[$k] syntax"));
      throw e;
    }
    case Collection::PairType: {
      Object e(SystemLib::AllocRuntimeExceptionObject(
        "Cannot assign to an element of a Pair"));
      throw e;
    }
    default:
      assert(false);
  }
  tvRefcountedDecRef(value);
}

void assertTv(const TypedValue* tv) {
  always_assert(tvIsPlausible(*tv));
}

int init_closure(ActRec* ar, TypedValue* sp) {
  c_Closure* closure = static_cast<c_Closure*>(ar->getThis());

  // Swap in the $this or late bound class
  ar->setThis(closure->getThisOrClass());

  if (ar->hasThis()) {
    ar->getThis()->incRefCount();
  }

  // Put in the correct context
  ar->m_func = closure->getInvokeFunc();

  // The closure is the first local.
  // Similar to tvWriteObject() but we don't incref because it used to be $this
  // and now it is a local, so they cancel out
  TypedValue* firstLocal = --sp;
  firstLocal->m_type = KindOfObject;
  firstLocal->m_data.pobj = closure;

  // Copy in all the use vars
  TypedValue* prop = closure->getUseVars();
  int n = closure->getNumUseVars();
  for (int i=0; i < n; i++) {
    tvDup(*prop++, *--sp);
  }

  return n + 1;
}

void raiseWarning(const StringData* sd) {
  raise_warning("%s", sd->data());
}

HOT_FUNC int64_t modHelper(int64_t left, int64_t right) {
  // We already dealt with divide-by-zero up in hhbctranslator.
  assert(right != 0);
  return left % right;
}

} // HPHP::VM

