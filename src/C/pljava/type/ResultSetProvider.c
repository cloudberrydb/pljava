/*
 * This file contains software that has been made available under
 * The Mozilla Public License 1.1. Use and distribution hereof are
 * subject to the restrictions set forth therein.
 *
 * Copyright (c) 2003 TADA AB - Taby Sweden
 * All Rights Reserved
 */
#include <postgres.h>
#include <utils/memutils.h>
#include <utils/numeric.h>
#include <nodes/execnodes.h>
#include <funcapi.h>

#include "pljava/type/Type_priv.h"
#include "pljava/type/NativeStruct.h"
#include "pljava/type/SingleRowWriter.h"
#include "pljava/HashMap.h"

/*
 * void primitive type.
 */
static jclass s_ResultSetProvider_class;
static jmethodID s_ResultSetProvider_assignRowValues;

static TypeClass s_ResultSetProviderClass;
static HashMap s_cache;

struct _CallContextData
{
	HashMap nativeCache;
	jobject singleRowWriter;
	jobject resultSetProvider;
};

typedef struct _CallContextData* CallContextData;

static Datum _ResultSetProvider_invoke(Type self, JNIEnv* env, jclass cls, jmethodID method, jvalue* args, PG_FUNCTION_ARGS)
{
	HashMap currentCache;
	CallContextData  ctxData;
    FuncCallContext* context;
	bool saveicj = isCallingJava;

	/* stuff done only on the first call of the function
	 */
	if(SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence
		 */
		context = SRF_FIRSTCALL_INIT();
		
		/* switch to memory context appropriate for multiple function calls
		 */
		MemoryContext oldContext = MemoryContextSwitchTo(context->multi_call_memory_ctx);

		/* Push a new NativeStruct cache so that NativeStructs created during
		 * the initial call will survive until SRF_RETURN_DONE.
		 */
		currentCache = NativeStruct_pushCache();

		/* Call the declared Java function. It returns the ResultSetProvider
		 * that later is used once for each row that should be obtained.
		 */
		isCallingJava = true;
		jobject tmp = (*env)->CallStaticObjectMethodA(env, cls, method, args);
		isCallingJava = saveicj;

		if(tmp == 0)
		{
			NativeStruct_popCache(env, currentCache);
			fcinfo->isnull = true;
			SRF_RETURN_DONE(context);
		}

		/* allocate a slot for a tuple with this tupdesc and assign it to
		 * the function context
		 */
		TupleDesc tupleDesc = TupleDesc_forOid(Type_getOid(self));
		context->slot = TupleDescGetSlot(tupleDesc);

		/* Create the context used by Pl/Java
		 */
		ctxData = (CallContextData)palloc(sizeof(struct _CallContextData));
		context->user_fctx = ctxData;

		/* Build a tuple description for the tuples
		 */
		ctxData->resultSetProvider = (*env)->NewGlobalRef(env, tmp);
		(*env)->DeleteLocalRef(env, tmp);

		tmp = SingleRowWriter_create(env, tupleDesc);		
		ctxData->singleRowWriter = (*env)->NewGlobalRef(env, tmp);
		(*env)->DeleteLocalRef(env, tmp);

		MemoryContextSwitchTo(oldContext);
		
		/* Switch NativeStruct cache so that the one just pushed is
		 * preserved.
		 */
		ctxData->nativeCache = NativeStruct_switchTopCache(currentCache);
	}

	context = SRF_PERCALL_SETUP();
	ctxData = (CallContextData)context->user_fctx;

	/* Obtain next row using the RowProvider as a parameter to the
	 * ResultSetProvider.assignRowValues method.
	 */
	isCallingJava = true;
	bool hasRow = ((*env)->CallBooleanMethod(
			env, ctxData->resultSetProvider, s_ResultSetProvider_assignRowValues,
			ctxData->singleRowWriter, (jint)context->call_cntr) == JNI_TRUE);
	isCallingJava = saveicj;

	if(hasRow)
	{
		/* Obtain tuple and return it as a Datum.
		 */
		HeapTuple tuple = SingleRowWriter_getTupleAndClear(env, ctxData->singleRowWriter);
	    Datum result = TupleGetDatum(context->slot, tuple);
		SRF_RETURN_NEXT(context, result);
	}

	/*
	 * This is the end of the set.
	 */
	(*env)->DeleteGlobalRef(env, ctxData->singleRowWriter);
	(*env)->DeleteGlobalRef(env, ctxData->resultSetProvider);

	/*
	 * Restore the preserved NativeStruct cache so that it becomes the
	 * one that is popped (and cleared) by next pop.
	 */
	currentCache = NativeStruct_switchTopCache(ctxData->nativeCache);

	/* Pop the NativeStruct cache and reinstate the one that we just
	 * switched out.
	 */
	NativeStruct_popCache(env, currentCache);
	pfree(ctxData);
	SRF_RETURN_DONE(context);
}

static jvalue _ResultSetProvider_coerceDatum(Type self, JNIEnv* env, Datum nothing)
{
	jvalue result;
	result.j = 0L;
	return result;
}

static Datum _ResultSetProvider_coerceObject(Type self, JNIEnv* env, jobject nothing)
{
	return 0;
}

static Type ResultSetProvider_obtain(Oid typeId)
{
	/* Check to see if we have a cached version for this
	 * postgres type
	 */
	Type infant = (Type)HashMap_getByOid(s_cache, typeId);
	if(infant == 0)
	{
		infant = TypeClass_allocInstance(s_ResultSetProviderClass, typeId);
		HashMap_putByOid(s_cache, typeId, infant);
	}
	return infant;
}

/* Make this datatype available to the postgres system.
 */
extern Datum ResultSetProvider_initialize(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(ResultSetProvider_initialize);
Datum ResultSetProvider_initialize(PG_FUNCTION_ARGS)
{
	JNIEnv* env = (JNIEnv*)PG_GETARG_POINTER(0);

	s_ResultSetProvider_class = (*env)->NewGlobalRef(
				env, PgObject_getJavaClass(env, "org/postgresql/pljava/ResultSetProvider"));

	s_ResultSetProvider_assignRowValues = PgObject_getJavaMethod(
				env, s_ResultSetProvider_class, "assignRowValues", "(Ljava/sql/ResultSet;I)Z");

	s_cache = HashMap_create(13, TopMemoryContext);

	s_ResultSetProviderClass = TypeClass_alloc("type.ResultSetProvider");
	s_ResultSetProviderClass->JNISignature = "Lorg/postgresql/pljava/ResultSetProvider;";
	s_ResultSetProviderClass->javaTypeName = "org.postgresql.pljava.ResultSetProvider";
	s_ResultSetProviderClass->invoke       = _ResultSetProvider_invoke;
	s_ResultSetProviderClass->coerceDatum  = _ResultSetProvider_coerceDatum;
	s_ResultSetProviderClass->coerceObject = _ResultSetProvider_coerceObject;

	Type_registerJavaType("org.postgresql.pljava.ResultSetProvider", ResultSetProvider_obtain);
	PG_RETURN_VOID();
}
