/*
 * Copyright (c) 2004, 2005, 2006 TADA AB - Taby Sweden
 * Copyright (c) 2007, 2008, 2010, 2011 PostgreSQL Global Development Group
 *
 * Distributed under the terms shown in the file COPYRIGHT
 * found in the root folder of this project or at
 * http://wiki.tada.se/index.php?title=PLJava_License
 *
 * @author Thomas Hallgren
 */
#include <sys/time.h>

#include <postgres.h>
#include <utils/datetime.h>

#include "pljava/Backend.h"
#include "pljava/type/Type_priv.h"
#include "pljava/type/Timestamp.h"

#define pg_unreachable() abort()

#define EPOCH_DIFF (((uint32)86400) * (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))

/*
 * Timestamp type. Postgres will pass (and expect in return) a local timestamp.
 * Java on the other hand has no object that represents local time (localization
 * is added when the object is converted to/from readable form). Hence, all
 * postgres timestamps must be converted from local time to UTC when passed as
 * a parameter to a Java method and all Java Timestamps must be converted from UTC
 * to localtime when returned to postgres.
 */
static jclass    s_Timestamp_class;
static jmethodID s_Timestamp_init;
static jmethodID s_Timestamp_getNanos;
static jmethodID s_Timestamp_getTime;
static jmethodID s_Timestamp_setNanos;

static TypeClass s_TimestampClass;
static TypeClass s_TimestamptzClass;

static bool _Timestamp_canReplaceType(Type self, Type other)
{
	TypeClass cls = Type_getClass(other);
	return Type_getClass(self) == cls || cls == s_TimestamptzClass;
}

static jvalue Timestamp_coerceDatumTZ_id(Type self, Datum arg, bool tzAdjust)
{
	jvalue result;
	int64 ts = DatumGetInt64(arg);
	int   tz = Timestamp_getTimeZone_id(ts);

	/* Expect number of microseconds since 01 Jan 2000
	 */
	jlong mSecs = ts / 1000; /* Convert to millisecs */
	jint  uSecs = (jint)(ts % 1000000); /* preserve microsecs */

	if(tzAdjust)
		mSecs += tz * 1000; /* Adjust from local time to UTC */

	/* Adjust for diff between Postgres and Java (Unix) */
	mSecs += ((jlong)EPOCH_DIFF) * 1000L;

	result.l = JNI_newObject(s_Timestamp_class, s_Timestamp_init, mSecs);
	if(uSecs != 0)
		JNI_callVoidMethod(result.l, s_Timestamp_setNanos, uSecs * 1000);
	return result;
}

static jvalue Timestamp_coerceDatumTZ_dd(Type self, Datum arg, bool tzAdjust)
{
	jlong mSecs;
	jint  uSecs;
	jvalue result;
	double ts = DatumGetFloat8(arg);
	int    tz = Timestamp_getTimeZone_dd(ts);

	/* Expect <seconds since Jan 01 2000>.<fractions of seconds>
	 */
	if(tzAdjust)
		ts += tz; /* Adjust from local time to UTC */
	ts += EPOCH_DIFF; /* Adjust for diff between Postgres and Java (Unix) */
	mSecs = (jlong) floor(ts * 1000.0); /* Convert to millisecs */
	uSecs = (jint) ((ts - floor(ts)) * 1000000.0); /* Preserve microsecs */
	result.l = JNI_newObject(s_Timestamp_class, s_Timestamp_init, mSecs);
	if(uSecs != 0)
		JNI_callVoidMethod(result.l, s_Timestamp_setNanos, uSecs * 1000);
	return result;
}

static jvalue Timestamp_coerceDatumTZ(Type self, Datum arg, bool tzAdjust)
{
	return integerDateTimes
		? Timestamp_coerceDatumTZ_id(self, arg, tzAdjust)
		: Timestamp_coerceDatumTZ_dd(self, arg, tzAdjust);
}

static Datum Timestamp_coerceObjectTZ_id(Type self, jobject jts, bool tzAdjust)
{
	int64 ts;
	jlong mSecs = JNI_callLongMethod(jts, s_Timestamp_getTime);
	jint  nSecs = JNI_callIntMethod(jts, s_Timestamp_getNanos);
	mSecs -= ((jlong)EPOCH_DIFF) * 1000L;
	ts  = mSecs * 1000L; /* Convert millisecs to microsecs */
	if(nSecs != 0)
		ts += nSecs / 1000;	/* Convert nanosecs  to microsecs */
	if(tzAdjust)
		ts -= ((jlong)Timestamp_getTimeZone_id(ts)) * 1000000L; /* Adjust from UTC to local time */
	return Int64GetDatum(ts);
}

static Datum Timestamp_coerceObjectTZ_dd(Type self, jobject jts, bool tzAdjust)
{
	double ts;
	jlong mSecs = JNI_callLongMethod(jts, s_Timestamp_getTime);
	jint  nSecs = JNI_callIntMethod(jts, s_Timestamp_getNanos);
	ts = ((double)mSecs) / 1000.0; /* Convert to seconds */
	ts -= EPOCH_DIFF;
	if(nSecs != 0)
		ts += ((double)nSecs) / 1000000000.0;	/* Convert to seconds */
	if(tzAdjust)
		ts -= Timestamp_getTimeZone_dd(ts); /* Adjust from UTC to local time */
	return Float8GetDatum(ts);
}

static Datum Timestamp_coerceObjectTZ(Type self, jobject jts, bool tzAdjust)
{
	return integerDateTimes
		? Timestamp_coerceObjectTZ_id(self, jts, tzAdjust)
		: Timestamp_coerceObjectTZ_dd(self, jts, tzAdjust);
}

static jvalue _Timestamp_coerceDatum(Type self, Datum arg)
{
	return Timestamp_coerceDatumTZ(self, arg, true);
}

static Datum _Timestamp_coerceObject(Type self, jobject ts)
{
	return Timestamp_coerceObjectTZ(self, ts, true);
}

/*
 * Timestamp with time zone. Basically same as Timestamp but postgres will pass
 * this one in GMT timezone so there's no without ajustment for time zone.
 */
static bool _Timestamptz_canReplaceType(Type self, Type other)
{
	TypeClass cls = Type_getClass(other);
	return Type_getClass(self) == cls || cls == s_TimestampClass;
}

static jvalue _Timestamptz_coerceDatum(Type self, Datum arg)
{
	return Timestamp_coerceDatumTZ(self, arg, false);
}

static Datum _Timestamptz_coerceObject(Type self, jobject ts)
{
	return Timestamp_coerceObjectTZ(self, ts, false);
}

static int32 Timestamp_getTimeZone(pg_time_t time)
{
#ifdef _MSC_VER
	/* This is gross, but pg_tzset has a cache, so not as gross as you think.
	 * There is some renewed interest on pgsql-hackers to find a good answer for
	 * the MSVC PGDLLIMPORT nonsense, so this may not have to stay gross.
	 */
	char const *tzname = PG_GETCONFIGOPTION("timezone");
	struct pg_tm* tx = pg_localtime(&time, pg_tzset(tzname));
#else
	struct pg_tm* tx = pg_localtime(&time, session_timezone);
#endif
	return -(int32)tx->tm_gmtoff;
}

int32 Timestamp_getTimeZone_id(int64 dt)
{
	return Timestamp_getTimeZone(
		(dt / INT64CONST(1000000) + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400));
}
int32 Timestamp_getTimeZone_dd(double dt)
{
	return Timestamp_getTimeZone(
		(pg_time_t)rint(dt + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400));
}

int32 Timestamp_getCurrentTimeZone(void)
{
	return Timestamp_getTimeZone((pg_time_t)time(NULL));
}

extern void Timestamp_initialize(void);
void Timestamp_initialize(void)
{
	TypeClass cls;
	s_Timestamp_class = JNI_newGlobalRef(PgObject_getJavaClass("java/sql/Timestamp"));
	s_Timestamp_init = PgObject_getJavaMethod(s_Timestamp_class, "<init>", "(J)V");
	s_Timestamp_getNanos = PgObject_getJavaMethod(s_Timestamp_class, "getNanos", "()I");
	s_Timestamp_getTime  = PgObject_getJavaMethod(s_Timestamp_class, "getTime",  "()J");
	s_Timestamp_setNanos = PgObject_getJavaMethod(s_Timestamp_class, "setNanos", "(I)V");

	cls = TypeClass_alloc("type.Timestamp");
	cls->JNISignature   = "Ljava/sql/Timestamp;";
	cls->javaTypeName   = "java.sql.Timestamp";
	cls->canReplaceType = _Timestamp_canReplaceType;
	cls->coerceDatum    = _Timestamp_coerceDatum;
	cls->coerceObject   = _Timestamp_coerceObject;
	Type_registerType(0, TypeClass_allocInstance(cls, TIMESTAMPOID));
	s_TimestampClass = cls;

	cls = TypeClass_alloc("type.Timestamptz");
	cls->JNISignature   = "Ljava/sql/Timestamp;";
	cls->javaTypeName   = "java.sql.Timestamp";
	cls->canReplaceType = _Timestamptz_canReplaceType;
	cls->coerceDatum    = _Timestamptz_coerceDatum;
	cls->coerceObject   = _Timestamptz_coerceObject;
	Type_registerType("java.sql.Timestamp", TypeClass_allocInstance(cls, TIMESTAMPTZOID));
	s_TimestamptzClass = cls;
}
