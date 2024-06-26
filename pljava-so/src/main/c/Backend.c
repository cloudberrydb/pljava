/*
 * Copyright (c) 2004, 2005, 2006 TADA AB - Taby Sweden
 * Copyright (c) 2009, 2010, 2011 PostgreSQL Global Development Group
 *
 * Distributed under the terms shown in the file COPYRIGHT
 * found in the root folder of this project or at
 * http://wiki.tada.se/index.php?title=PLJava_License
 *
 * @author Thomas Hallgren
 */
#include <postgres.h>
#include <miscadmin.h>
#ifndef WIN32
#include <libpq/pqsignal.h>
#endif
#include <executor/spi.h>
#include <commands/trigger.h>
#include <utils/elog.h>
#include <utils/guc.h>
#include <fmgr.h>
#include <access/heapam.h>
#include <utils/syscache.h>
#include <catalog/catalog.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>

#if PG_VERSION_NUM >= 120000
 #ifdef HAVE_DLOPEN
 #include <dlfcn.h>
 #endif
 #define pg_dlopen(f) dlopen((f), RTLD_NOW | RTLD_GLOBAL)
 #define pg_dlsym(h,s) dlsym((h), (s))
 #define pg_dlclose(h) dlclose((h))
 #define pg_dlerror() dlerror()
#else
 #include <dynloader.h>
#endif

#include <storage/ipc.h>
#include <storage/proc.h>
#include <storage/sinval.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#include "org_postgresql_pljava_internal_Backend.h"
#include "pljava/Invocation.h"
#include "pljava/InstallHelper.h"
#include "pljava/Function.h"
#include "pljava/HashMap.h"
#include "pljava/Exception.h"
#include "pljava/Backend.h"
#include "pljava/Session.h"
#include "pljava/SPI.h"
#include "pljava/type/String.h"

#if PG_VERSION_NUM >= 90300
#include "utils/timeout.h"
#endif

#define pg_unreachable() abort()

/* Include the 'magic block' that PostgreSQL 8.2 and up will use to ensure
 * that a module is not loaded into an incompatible server.
 */ 
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/* About PGDLLEXPORT. It didn't exist before PG 9.0. In that revision it was
 * defined (for Windows) as __declspec(dllexport) for MSVC, but as
 * __declspec(dllimport) for any other toolchain. That was quickly changed
 * (for 9.0.2 and ever since) as still __declspec(dllexport) for MSVC, but
 * empty for any other toolchain. The explanation for that (in PG commit
 * 844ed5d in November 2010) was that "dllexport and dllwrap don't work well
 * together." There are records as far back as 2002 anyway
 * (e.g. http://lists.gnu.org/archive/html/libtool/2002-09/msg00069.html)
 * calling dllwrap deprecated, PL/Java's Maven build certainly doesn't
 * use it, and I don't know what it would do if it did. It seems too brittle
 * to rely on whatever PGDLLEXPORT might happen to mean across PG versions,
 * and wiser for the moment to cleanly define something here, for the all of
 * three(*) symbols that need it.
 *
 * The only case where it expands to anything is when building with Microsoft
 * Visual Studio. When building with other toolchains it just goes away, even
 * on Windows when building with MinGW (the only other Windows toolchain
 * tested). MinGW can work either way: selectively exporting things based on
 * a __declspec, or with the --export-all-symbols linker option so everything
 * is visible, as on a *n*x platform. PL/Java could in theory choose either
 * approach, but for one detail: there is a (*)fourth symbol that needs to be
 * exported. PG_MODULE_MAGIC defines one, and being a PostgreSQL-supplied macro,
 * it uses PGDLLEXPORT, which expands to nothing for MinGW (in recent PG
 * versions anyway), forcing --export-all-symbols as the answer for MinGW.
 */
#ifdef _MSC_VER
#define PLJAVADLLEXPORT __declspec (dllexport)
#else
#define PLJAVADLLEXPORT
#endif

extern PLJAVADLLEXPORT void _PG_init(void);

#define LOCAL_REFERENCE_COUNT 128

jlong mainThreadId;

static pthread_t s_mainThreadIdForHandler;
static bool s_handlerSubstituted = false;
static pqsigfunc s_oldHandlerFunc = NULL;

static JavaVM* s_javaVM = 0;
static jclass  s_Backend_class;
static jmethodID s_setTrusted;
static char* libjvmlocation;
static int   statementCacheSize;
static bool  pljavaReleaseLingeringSavepoints;
static bool  s_currentTrust;
static int   s_javaLogLevel;

bool integerDateTimes = false;

extern void Invocation_initialize(void);
extern void Exception_initialize(void);
extern void Exception_initialize2(void);
extern void HashMap_initialize(void);
extern void SPI_initialize(void);
extern void Type_initialize(void);
extern void Function_initialize(void);
extern void Session_initialize(void);
extern void PgSavepoint_initialize(void);
extern void XactListener_initialize(void);
extern void SubXactListener_initialize(void);
extern void SQLInputFromChunk_initialize(void);
extern void SQLOutputToChunk_initialize(void);
extern void SQLInputFromTuple_initialize(void);
extern void SQLOutputToTuple_initialize(void);


typedef struct {
	JavaVMOption* options;
	unsigned int  size;
	unsigned int  capacity;
} JVMOptList;

static jint initializeJavaVM(JVMOptList*);
static void JVMOptList_init(JVMOptList*);
static void JVMOptList_delete(JVMOptList*);
static void JVMOptList_add(JVMOptList*, const char*, void*, bool);
static void JVMOptList_addVisualVMName(JVMOptList*);
static void addUserJVMOptions(JVMOptList*);
static void checkIntTimeType(void);
static char* getClassPath(const char*);
static jint JNICALL my_vfprintf(FILE*, const char*, va_list);
static void _destroyJavaVM(int, Datum);
static void initPLJavaClasses(void);
static void initJavaSession(void);
static void reLogWithChangedLevel(int);

#ifndef WIN32
#define USE_PLJAVA_SIGHANDLERS
#endif

#ifdef USE_PLJAVA_SIGHANDLERS
static void pljavaStatementCancelHandler(int);
static void pljavaDieHandler(int);
#endif

enum initstage
{
	IS_FORMLESS_VOID,
	IS_GUCS_REGISTERED,
	IS_CAND_JVMLOCATION,
	IS_PLJAVA_ENABLED,
	IS_CAND_JVMOPENED,
	IS_CREATEVM_SYM_FOUND,
	IS_MISC_ONCE_DONE,
	IS_JAVAVM_OPTLIST,
	IS_JAVAVM_STARTED,
	IS_SIGHANDLERS,
	IS_PLJAVA_FOUND,
	IS_COMPLETE
};

static enum initstage initstage = IS_FORMLESS_VOID;
static void *libjvm_handle;
static bool jvmStartedAtLeastOnce = false;
static bool alteredSettingsWereNeeded = false;
static bool loadAsExtensionFailed = false;
static bool seenVisualVMName;
static char const visualVMprefix[] = "-Dvisualvm.display.name=";

static void initsequencer(enum initstage is, bool tolerant);

/*
 * There are a few ways to arrive in the initsequencer.
 * 1. From _PG_init (called exactly once when the library is loaded for ANY
 *    reason).
 *    1a. Because of the command LOAD 'libraryname';
 *        This case can be distinguished because _PG_init will have found the
 *        LOAD command and saved the 'libraryname' in pljavaLoadPath.
 *    1b. Because of a CREATE FUNCTION naming this library. pljavaLoadPath will
 *        be NULL.
 *    1c. By the first actual use of a PL/Java function, causing this library
 *        to be loaded. pljavaLoadPath will be NULL. The called function's Oid
 *        will be available to the call handler once we return from _PG_init,
 *        but it isn't (easily) available here.
 * 2. From the call handler, if initialization isn't complete yet. That can only
 *    mean something failed in the earlier call to _PG_init, and whatever it was
 *    is highly likely to fail again. That may lead to the untidyness of
 *    duplicated diagnostic messages, but for now I like the belt-and-suspenders
 *    approach of making sure the init sequence gets as many chances as possible
 *    to succeed.
 * 3. From a GUC assign hook, if the user has updated a setting that might allow
 *    initialization to succeed. It resumes from where it left off.
 *
 * In all cases, the sequence must progress as far as starting the VM and
 * initializing the PL/Java classes. In all cases except 1a, that's enough,
 * assuming the language handlers and schema have all been set up already (or,
 * in case 1b, the user is intent on setting them up explicitly).
 *
 * In case 1a, we can go ahead and test for, and create, the schema, functions,
 * and language entries as needed, using pljavaLoadPath as the library path
 * if creating the language handler functions. One-stop shopping. (The presence
 * of pljavaLoadPath in any of the other cases, such as resumption by an assign
 * hook, indicates it is really a continuation of case 1a.)
 */
static void initsequencer(enum initstage is, bool tolerant)
{
	JVMOptList optList;
	Invocation ctx;
	jint JNIresult;
	char *greeting;

	switch (is)
	{
	case IS_FORMLESS_VOID:
		initstage = IS_GUCS_REGISTERED;

	case IS_GUCS_REGISTERED:
		libjvmlocation = strdup("libjvm.so");

		initstage = IS_PLJAVA_ENABLED;

	case IS_PLJAVA_ENABLED:
		libjvm_handle = pg_dlopen(libjvmlocation);
		if ( NULL == libjvm_handle )
		{
			ereport(ERROR, (
				errmsg("Cannot load libjvm.so library, check that it is available in LD_LIBRARY_PATH"),
				errdetail("%s", (char *)pg_dlerror())));
			goto check_tolerant;
		}
		initstage = IS_CAND_JVMOPENED;

	case IS_CAND_JVMOPENED:
		pljava_createvm =
			(jint (JNICALL *)(JavaVM **, void **, void *))
			pg_dlsym(libjvm_handle, "JNI_CreateJavaVM");
		if ( NULL == pljava_createvm )
		{
			/*
			 * If it hasn't got the symbol, it can't be the right
			 * library, so close/unload it so another can be tried.
			 * Format the dlerror string first: dlclose may clobber it.
			 */
			char *dle = MemoryContextStrdup(ErrorContext, pg_dlerror());
			pg_dlclose(libjvm_handle);
			initstage = IS_CAND_JVMLOCATION;
			ereport(ERROR, (
				errmsg("Cannot start Java VM"),
				errdetail("%s", dle),
				errhint("Check that libjvm.so is available in LD_LIBRARY_PATH")));
			goto check_tolerant;
		}
		initstage = IS_CREATEVM_SYM_FOUND;

	case IS_CREATEVM_SYM_FOUND:
		s_javaLogLevel = INFO;
		checkIntTimeType();
		HashMap_initialize(); /* creates things in TopMemoryContext */
#ifdef PLJAVA_DEBUG
		/* Hard setting for debug. Don't forget to recompile...
		 */
		pljava_debug = 1;
#endif
		initstage = IS_MISC_ONCE_DONE;

	case IS_MISC_ONCE_DONE:
		JVMOptList_init(&optList); /* uses CurrentMemoryContext */
		seenVisualVMName = false;
		addUserJVMOptions(&optList);
		if ( ! seenVisualVMName )
			JVMOptList_addVisualVMName(&optList);
		JVMOptList_add(&optList, "vfprintf", (void*)my_vfprintf, true);
		JVMOptList_add(&optList, "-Xss2m", 0, true);
#ifndef GCJ
		JVMOptList_add(&optList, "-Xrs", 0, true);
#endif
		effectiveClassPath = getClassPath("-Djava.class.path=");
		if(effectiveClassPath != 0)
		{
			JVMOptList_add(&optList, effectiveClassPath, 0, true);
		}
		initstage = IS_JAVAVM_OPTLIST;

	case IS_JAVAVM_OPTLIST:
		JNIresult = initializeJavaVM(&optList); /* frees the optList */
		if( JNI_OK != JNIresult )
		{
			initstage = IS_MISC_ONCE_DONE; /* optList has been freed */
			StaticAssertStmt(sizeof(jint) <= sizeof(long int),
				"jint wider than long int?!");
			ereport(WARNING,
				(errmsg("failed to create Java virtual machine"),
				 errdetail("JNI_CreateJavaVM returned an error code: %ld",
					(long int)JNIresult),
				 jvmStartedAtLeastOnce ?
					errhint("Because an earlier attempt during this session "
					"did start a VM before failing, this probably means your "
					"Java runtime environment does not support more than one "
					"VM creation per session.  You may need to exit this "
					"session and start a new one.") : 0));
			goto check_tolerant;
		}
		jvmStartedAtLeastOnce = true;
		elog(DEBUG2, "successfully created Java virtual machine");
		initstage = IS_JAVAVM_STARTED;

	case IS_JAVAVM_STARTED:
#ifdef USE_PLJAVA_SIGHANDLERS
		pqsignal(SIGINT,  pljavaStatementCancelHandler);
		pqsignal(SIGTERM, pljavaDieHandler);
#endif
		/* Register an on_proc_exit handler that destroys the VM
		 */
		on_proc_exit(_destroyJavaVM, 0);
		initstage = IS_SIGHANDLERS;

	case IS_SIGHANDLERS:
		Invocation_pushBootContext(&ctx);
		PG_TRY();
		{
			initPLJavaClasses();
			initJavaSession();
			Invocation_popBootContext();
			initstage = IS_PLJAVA_FOUND;
		}
		PG_CATCH();
		{
			MemoryContextSwitchTo(ctx.upperContext); /* leave ErrorContext */
			Invocation_popBootContext();
			initstage = IS_MISC_ONCE_DONE;
			/* We can't stay here...
			 */
			if ( tolerant )
				reLogWithChangedLevel(WARNING); /* so xact is not aborted */
			else
			{
				EmitErrorReport(); /* no more unwinding, just log it */
				/* Seeing an ERROR emitted to the log, without leaving the
				 * transaction aborted, would violate the principle of least
				 * astonishment. But at check_tolerant below, another ERROR will
				 * be thrown immediately, so the transaction effect will be as
				 * expected and this ERROR will contribute information beyond
				 * what is in the generic one thrown down there.
				 */
				FlushErrorState();
			}
		}
		PG_END_TRY();
		if ( IS_PLJAVA_FOUND != initstage )
		{
			/* JVM initialization failed for some reason. Destroy
			 * the VM if it exists. Perhaps the user will try
			 * fixing the pljava.classpath and make a new attempt.
			 */
			ereport(WARNING, (
				errmsg("failed to load initial PL/Java classes"),
				errhint("The most common reason is that \"pljava_classpath\" "
					"needs to be set, naming the proper \"pljava.jar\" file.")
					));
			_destroyJavaVM(0, 0);
			goto check_tolerant;
		}

	case IS_PLJAVA_FOUND:
		greeting = InstallHelper_hello();
		ereport(NULL != pljavaLoadPath ? NOTICE : DEBUG1, (
				errmsg("PL/Java loaded"),
				errdetail("versions:\n%s", greeting)));
		pfree(greeting);
		if ( NULL != pljavaLoadPath )
			InstallHelper_groundwork(); /* sqlj schema, language handlers, ...*/
		initstage = IS_COMPLETE;

	case IS_COMPLETE:
		pljavaLoadingAsExtension = false;
		if ( alteredSettingsWereNeeded )
		{
			/* Use this StringInfoData to conditionally construct part of the
			 * hint string suggesting ALTER DATABASE ... SET ... FROM CURRENT
			 * provided the server is >= 9.2 where that will actually work.
			 * In 9.3, psprintf appeared, which would make this all simpler,
			 * but if 9.3+ were all that had to be supported, this would all
			 * be moot anyway. Doing the initStringInfo inside the ereport
			 * ensures the string is allocated in ErrorContext and won't leak.
			 * Don't remove the extra parens grouping
			 * (initStringInfo, appendStringInfo, errhint) ... with the parens,
			 * that's a comma expression, which is sequenced; without them, they
			 * are just function parameters with evaluation order unknown.
			 */
			StringInfoData buf;
#if PG_VERSION_NUM >= 90200
#define MOREHINT \
				appendStringInfo(&buf, \
					"using ALTER DATABASE %s SET ... FROM CURRENT or ", \
					pljavaDbName()),
#else
#define MOREHINT
#endif
			ereport(NOTICE, (
				errmsg("PL/Java successfully started after adjusting settings"),
				(initStringInfo(&buf),
				MOREHINT
				errhint("The settings that worked should be saved (%s"
					"in the \"%s\" file). For a reminder of what has been set, "
					"try: SELECT name, setting FROM pg_settings WHERE name LIKE"
					" 'pljava.%%' AND source = 'session'",
					buf.data,
					superuser()
						? PG_GETCONFIGOPTION("config_file")
						: "postgresql.conf"))));
#undef MOREHINT
			if ( loadAsExtensionFailed )
			{
				ereport(NOTICE, (errmsg(
					"PL/Java load successful after failed CREATE EXTENSION"),
					errdetail(
					"PL/Java is now installed, but not as an extension."),
					errhint(
					"To correct that, either COMMIT or ROLLBACK, make sure "
					"the working settings are saved, exit this session, and "
					"in a new session, either: "
					"1. if committed, run "
					"\"CREATE EXTENSION pljava FROM unpackaged\", or 2. "
					"if rolled back, simply \"CREATE EXTENSION pljava\" again."
					)));
			}
		}
		return;

	default:
		ereport(ERROR, (
			errmsg("cannot set up PL/Java"),
			errdetail(
				"An unexpected stage was reached in the startup sequence."),
			errhint(
				"Please report the circumstances to the PL/Java maintainers.")
			));
	}

check_tolerant:
	if ( pljavaLoadingAsExtension )
	{
		tolerant = false;
		loadAsExtensionFailed = true;
		pljavaLoadingAsExtension = false;
	}
	if ( !tolerant )
	{
		ereport(ERROR, (
			errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			errmsg(
				"cannot use PL/Java before successfully completing its setup"),
			errhint(
				"Check the log for messages closely preceding this one, "
				"detailing what step of setup failed and what will be needed, "
				"probably setting one of the \"pljava.\" configuration "
				"variables, to complete the setup. If there is not enough "
				"help in the log, try again with different settings for "
				"\"log_min_messages\" or \"log_error_verbosity\".")));
	}
}

/*
 * A function having everything to do with logging, which ought to be factored
 * out one day to make a start on the Thoughts-on-logging wiki ideas.
 */
static void reLogWithChangedLevel(int level)
{
	ErrorData *edata = CopyErrorData();
	int sqlstate = edata->sqlerrcode;
	int category = ERRCODE_TO_CATEGORY(sqlstate);
	FlushErrorState();
	if ( WARNING > level )
	{
		if ( ERRCODE_SUCCESSFUL_COMPLETION != category )
			sqlstate = ERRCODE_SUCCESSFUL_COMPLETION;
	}
	else if ( WARNING == level )
	{
		if ( ERRCODE_WARNING != category && ERRCODE_NO_DATA != category )
			sqlstate = ERRCODE_WARNING;
	}
	else if ( ERRCODE_WARNING == category || ERRCODE_NO_DATA == category ||
		ERRCODE_SUCCESSFUL_COMPLETION == category )
		sqlstate = ERRCODE_INTERNAL_ERROR;
#if PG_VERSION_NUM >= 90500
	edata->elevel = level;
	edata->sqlerrcode = sqlstate;
	PG_TRY();
	{
		ThrowErrorData(edata);
	}
	PG_CATCH();
	{
		FreeErrorData(edata); /* otherwise this wouldn't happen in ERROR case */
		PG_RE_THROW();
	}
	PG_END_TRY();
	FreeErrorData(edata);
#else
	if (!errstart(level, edata->filename, edata->lineno,
				  edata->funcname, NULL))
	{
		FreeErrorData(edata);
		return;
	}

	errcode(sqlstate);
	if (edata->message)
		errmsg("%s", edata->message);
	if (edata->detail)
		errdetail("%s", edata->detail);
	if (edata->detail_log)
		errdetail_log("%s", edata->detail_log);
	if (edata->hint)
		errhint("%s", edata->hint);
	if (edata->context)
		errcontext("%s", edata->context); /* this may need to be trimmed */
#if PG_VERSION_NUM >= 90300
	if (edata->schema_name)
		err_generic_string(PG_DIAG_SCHEMA_NAME, edata->schema_name);
	if (edata->table_name)
		err_generic_string(PG_DIAG_TABLE_NAME, edata->table_name);
	if (edata->column_name)
		err_generic_string(PG_DIAG_COLUMN_NAME, edata->column_name);
	if (edata->datatype_name)
		err_generic_string(PG_DIAG_DATATYPE_NAME, edata->datatype_name);
	if (edata->constraint_name)
		err_generic_string(PG_DIAG_CONSTRAINT_NAME, edata->constraint_name);
#endif
	if (edata->internalquery)
		internalerrquery(edata->internalquery);

	FreeErrorData(edata);
	errfinish(0);
#endif
}

void _PG_init()
{
	if ( IS_PLJAVA_FOUND == initstage )
		return; /* creating handler functions will cause recursive call */
	pljavaCheckExtension( NULL);
	initsequencer( initstage, true);
}

static void initPLJavaClasses(void)
{
	jfieldID tlField;
	JNINativeMethod backendMethods[] =
	{
		{
		"isCallingJava",
	  	"()Z",
	  	Java_org_postgresql_pljava_internal_Backend_isCallingJava
		},
		{
		"isReleaseLingeringSavepoints",
	  	"()Z",
	  	Java_org_postgresql_pljava_internal_Backend_isReleaseLingeringSavepoints
		},
		{
		"_getLibraryPath",
		"()Ljava/lang/String;",
		Java_org_postgresql_pljava_internal_Backend__1getLibraryPath
		},
		{
		"_getConfigOption",
		"(Ljava/lang/String;)Ljava/lang/String;",
		Java_org_postgresql_pljava_internal_Backend__1getConfigOption
		},
		{
		"_getStatementCacheSize",
		"()I",
		Java_org_postgresql_pljava_internal_Backend__1getStatementCacheSize
		},
		{
		"_log",
		"(ILjava/lang/String;)V",
		Java_org_postgresql_pljava_internal_Backend__1log
		},
		{
		"_clearFunctionCache",
		"()V",
		Java_org_postgresql_pljava_internal_Backend__1clearFunctionCache
		},
		{
		"_isCreatingExtension",
		"()Z",
		Java_org_postgresql_pljava_internal_Backend__1isCreatingExtension
		},
		{ 0, 0, 0 }
	};

	Exception_initialize();

	elog(DEBUG2, "checking for a PL/Java Backend class on the given classpath");
	s_Backend_class = PgObject_getJavaClass(
		"org/postgresql/pljava/internal/Backend");
	elog(DEBUG2, "successfully loaded Backend class");
	PgObject_registerNatives2(s_Backend_class, backendMethods);

	tlField = PgObject_getStaticJavaField(s_Backend_class, "THREADLOCK", "Ljava/lang/Object;");
	JNI_setThreadLock(JNI_getStaticObjectField(s_Backend_class, tlField));

	Invocation_initialize();
	Exception_initialize2();
	SPI_initialize();
	Type_initialize();
	Function_initialize();
	Session_initialize();
	PgSavepoint_initialize();
	XactListener_initialize();
	SubXactListener_initialize();
	SQLInputFromChunk_initialize();
	SQLOutputToChunk_initialize();
	SQLInputFromTuple_initialize();
	SQLOutputToTuple_initialize();

	InstallHelper_initialize();

	s_setTrusted = PgObject_getStaticJavaMethod(s_Backend_class, "setTrusted", "(Z)V");
}

/**
 *  Initialize security
 */
void Backend_setJavaSecurity(bool trusted)
{
	if(trusted != s_currentTrust)
	{
		/* GCJ has major issues here. Real work on SecurityManager and
		 * related classes has just started in version 4.0.0.
		 */
#ifndef GCJ
		JNI_callStaticVoidMethod(s_Backend_class, s_setTrusted, (jboolean)trusted);
		if(JNI_exceptionCheck())
		{
			JNI_exceptionDescribe();
			JNI_exceptionClear();
			ereport(ERROR, (
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("Unable to initialize java security")));
		}
#endif
		s_currentTrust = trusted;
	}
}

int Backend_setJavaLogLevel(int logLevel)
{
	int oldLevel = s_javaLogLevel;
	s_javaLogLevel = logLevel;
	return oldLevel;
}
	
/**
 * Special purpose logging function called from JNI when verbose is enabled.
 */
static jint JNICALL my_vfprintf(FILE* fp, const char* format, va_list args)
{
	char buf[1024];
	char* ep;
	char* bp = buf;

    vsnprintf(buf, sizeof(buf), format, args);

    /* Trim off trailing newline and other whitespace.
     */
	ep = bp + strlen(bp) - 1;
    while(ep >= bp && isspace(*ep))
 		--ep;
 	++ep;
 	*ep = 0;

    elog(s_javaLogLevel, "%s", buf);
    return 0;
}

/*
 * Append those parts of path that has not yet been appended. The HashMap unique is
 * keeping track of what has been appended already. First appended part will be
 * prefixed with prefix.
 */
static void appendPathParts(const char* path, StringInfoData* bld, HashMap unique, const char* prefix)
{
	StringInfoData buf;
	if(path == 0 || strlen(path) == 0)
		return;

	for (;;)
	{
		char* pathPart;
		size_t len;
		if(*path == 0)
			break;

		len = strcspn(path, ";:");

		if(len == 1 && *(path+1) == ':' && isalnum(*path))
			/*
			 * Windows drive designator, leave it "as is".
			 */
			len = strcspn(path+2, ";:") + 2;
		else
		if(len == 0)
			{
			/* Ignore zero length components.
			 */
			++path;
			continue;
			}

		initStringInfo(&buf);
		if(*path == '$')
		{
			if(len == 7 || (strcspn(path, "/\\") == 7 && strncmp(path, "$libdir", 7) == 0))
			{
				char pathbuf[MAXPGPATH];
				get_pkglib_path(my_exec_path, pathbuf);
				len -= 7;
				path += 7;
				appendStringInfoString(&buf, pathbuf);
			}
			else
				ereport(ERROR, (
					errcode(ERRCODE_INVALID_NAME),
					errmsg("invalid macro name '%*s' in PL/Java classpath", (int)len, path)));
		}

		if(len > 0)
		{
			appendBinaryStringInfo(&buf, path, (int)len);
			path += len;
		}

		pathPart = buf.data;
		if(HashMap_getByString(unique, pathPart) == 0)
		{
			if(HashMap_size(unique) == 0)
				appendStringInfo(bld, "%s", prefix);
			else
#if defined(WIN32)
				appendStringInfoChar(bld, ';');
#else
				appendStringInfoChar(bld, ':');
#endif
			appendStringInfo(bld, "%s", pathPart);
			HashMap_putByString(unique, pathPart, (void*)1);
		}
		pfree(pathPart);
		if(*path == 0)
			break;
		++path; /* Skip ':' */
	}
}

/*
 * Get the CLASSPATH. Result is always freshly palloc'd.
 */
static char* getClassPath(const char* prefix)
{
	char* path;
	HashMap unique = HashMap_create(13, CurrentMemoryContext);
	StringInfoData buf;
	initStringInfo(&buf);

	/* Put the pljava installed in the $libdir first in the path */
	appendPathParts("$libdir/java/pljava.jar", &buf, unique, prefix);

#if 0
	/*
	 * Currently pljava.classpath is user setable, which makes this a
	 * security problem.  If CLASSPATH needs to be setable beyond simply
	 * locating the pljava.jar file then this requires modification.
	 *
	 * The Greenplum version of pljava currently uses the classpath guc
	 * differently anyhow due to differences in storing the jar files
	 * in the filesystem rather than in the database.
	 */
	appendPathParts(pljava_classpath, &buf, unique, prefix);

	/*
	 * For this to be useful it needs to be propagated from the
	 * master to all the segments, otherwise it wouldn't be the
	 * same everyplace and that would be a problem.
	 *
	 * Using a jvm_classpath GUC makes more architectural sense,
	 * for it to be secure it would need to be super-user only,
	 * possibly conf file only.
	 */
	appendPathParts(getenv("CLASSPATH"), &buf, unique, prefix);
#endif

	PgObject_free((PgObject)unique);
	path = buf.data;
	if(strlen(path) == 0)
	{
		pfree(path);
		path = 0;
	}
	return path;
}

#ifdef USE_PLJAVA_SIGHANDLERS

static void pljavaStatementCancelHandler(int signum)
{
	if(!proc_exit_inprogress)
	{
		/* Never service the interrupt immediately. In order to find out if
		 * its safe, we would need to know what kind of threading mechanism
		 * the VM uses. That would count for a lot of conditional code.
		 */
		QueryCancelPending = true;
		InterruptPending = true;
	}
}

static void pljavaDieHandler(int signum)
{
	if(!proc_exit_inprogress)
	{
		/* Never service the interrupt immediately. In order to find out if
		 * its safe, we would need to know what kind of threading mechanism
		 * the VM uses. That would count for a lot of conditional code.
		 */
		ProcDiePending = true;
		InterruptPending = true;
	}
}

static void PLCatchupInterruptHandler(int signo)
{
	if(s_oldHandlerFunc == NULL)
	{
		return;
	}

	pthread_t currentThreadId = pthread_self();

	if(currentThreadId == s_mainThreadIdForHandler)
	{
		s_oldHandlerFunc(signo);
	}
	else
	{
		pthread_kill(s_mainThreadIdForHandler, signo);
	}
}

static sigjmp_buf recoverBuf;
static void terminationTimeoutHandler(
#if PG_VERSION_NUM >= 90300
#else
	int signum
#endif
)
{
	kill(MyProcPid, SIGQUIT);
	
	/* Some sleep to get the SIGQUIT a chance to generate
	 * the needed output.
	 */
	pg_usleep(1);

	/* JavaVM did not die within the alloted time
	 */
	siglongjmp(recoverBuf, 1);
}
#endif

/*
 * proc_exit callback to tear down the JVM
 */
static void _destroyJavaVM(int status, Datum dummy)
{
	if(s_javaVM != 0)
	{
		Invocation ctx;
#ifdef USE_PLJAVA_SIGHANDLERS

#if PG_VERSION_NUM >= 90300
		TimeoutId tid;
#else
		pqsigfunc saveSigAlrm;
#endif

		Invocation_pushInvocation(&ctx, false);
		if(sigsetjmp(recoverBuf, 1) != 0)
		{
			elog(DEBUG2,
				"needed to forcibly shut down the Java virtual machine");
			s_javaVM = 0;
			return;
		}

#if PG_VERSION_NUM >= 90300
		InitializeTimeouts();           /* establishes SIGALRM handler */
		tid = RegisterTimeout(USER_TIMEOUT, terminationTimeoutHandler);
#else
		saveSigAlrm = pqsignal(SIGALRM, terminationTimeoutHandler);
		enable_sig_alarm(5000, false);
#endif

		elog(DEBUG2, "shutting down the Java virtual machine");
		JNI_destroyVM(s_javaVM);

#if PG_VERSION_NUM >= 90300
		disable_timeout(tid, false);
#else
		disable_sig_alarm(false);
		pqsignal(SIGALRM, saveSigAlrm);
#endif

#else
		Invocation_pushInvocation(&ctx, false);
		elog(DEBUG2, "shutting down the Java virtual machine");
		JNI_destroyVM(s_javaVM);
#endif
		elog(DEBUG2, "done shutting down the Java virtual machine");
		s_javaVM = 0;
		currentInvocation = 0;

		/*
		 * Recover the handler of SIGUSR1 when initiate JVM failed.
		 * */
		if(s_oldHandlerFunc == NULL)
		{
			pqsignal(SIGUSR1, SIG_IGN);
		}
		else
		{
			pqsignal(SIGUSR1, s_oldHandlerFunc);
		}
		s_handlerSubstituted = false;
	}
}

static void JVMOptList_init(JVMOptList* jol)
{
	jol->options  = (JavaVMOption*)palloc(10 * sizeof(JavaVMOption));
	jol->size     = 0;
	jol->capacity = 10;
}

static void JVMOptList_delete(JVMOptList* jol)
{
	JavaVMOption* opt = jol->options;
	JavaVMOption* top = opt + jol->size;
	while(opt < top)
	{
		pfree(opt->optionString);
		opt++;
	}
	pfree(jol->options);
}

static void JVMOptList_add(JVMOptList* jol, const char* optString, void* extraInfo, bool mustCopy)
{
	JavaVMOption* added;

	int newPos = jol->size;
	if(newPos >= jol->capacity)
	{
		int newCap = jol->capacity * 2;
		JavaVMOption* newOpts = (JavaVMOption*)palloc(newCap * sizeof(JavaVMOption));
		memcpy(newOpts, jol->options, newPos * sizeof(JavaVMOption));
		pfree(jol->options);
		jol->options = newOpts;
		jol->capacity = newCap;
	}
	added = jol->options + newPos;
	if(mustCopy)
		optString = pstrdup(optString);

	added->optionString = (char*)optString;
	added->extraInfo    = extraInfo;
	jol->size++;

	if ( 0 == strncmp(optString, visualVMprefix, sizeof visualVMprefix - 1) )
		seenVisualVMName = true;

	elog(DEBUG2, "Added JVM option string \"%s\"", optString);
}

static void JVMOptList_addVisualVMName(JVMOptList* jol)
{
	char const *clustername = pljavaClusterName();
	StringInfoData buf;
	initStringInfo(&buf);
	if ( '\0' == *clustername )
		appendStringInfo(&buf, "%sPL/Java:%d:%s",
			visualVMprefix, MyProcPid, pljavaDbName());
	else
		appendStringInfo(&buf, "%sPL/Java:%s:%d:%s",
			visualVMprefix, clustername, MyProcPid, pljavaDbName());
	JVMOptList_add(jol, buf.data, 0, false);
}

/* Split JVM options. The string is split on whitespace unless the
 * whitespace is found within a string or is escaped by backslash. A
 * backslash escaped quote is not considered a string delimiter.
 */
static void addUserJVMOptions(JVMOptList* optList)
{
	const char* cp = pljava_vmoptions;
	
	if(cp != NULL)
	{
		StringInfoData buf;
		char quote = 0;
		char c;

		initStringInfo(&buf);
		for(;;)
		{
			c = *cp++;
			switch(c)
			{
				case 0:
					break;

				case '"':
				case '\'':
					if(quote == c)
						quote = 0;
					else
						quote = c;
					appendStringInfoChar(&buf, c);
					continue;

				case '\\':
					appendStringInfoChar(&buf, '\\');
					c = *cp++;	/* Interpret next character verbatim */
					if(c == 0)
						break;
					appendStringInfoChar(&buf, c);
					continue;
					
				default:
					if(quote == 0 && isspace((int)c))
					{
						while((c = *cp++) != 0)
						{
							if(!isspace((int)c))
								break;
						}

						if(c == 0)
							break;

						if(c != '-')
							appendStringInfoChar(&buf, ' ');
						else if(buf.len > 0)
						{
							/* Whitespace followed by '-' triggers new
							 * option declaration.
							 */
							JVMOptList_add(optList, buf.data, 0, true);
							buf.len = 0;
							buf.data[0] = 0;
						}
					}
					appendStringInfoChar(&buf, c);
					continue;
			}
			break;
		}
		if(buf.len > 0)
			JVMOptList_add(optList, buf.data, 0, true);
		pfree(buf.data);
	}
}

/**
 *  Initialize the session
 */
static void initJavaSession(void)
{
	jclass sessionClass = PgObject_getJavaClass("org/postgresql/pljava/internal/Session");
	jmethodID init = PgObject_getStaticJavaMethod(sessionClass, "init", "()J");
	mainThreadId = JNI_callStaticLongMethod(sessionClass, init);
	JNI_deleteLocalRef(sessionClass);

	if(JNI_exceptionCheck())
	{
		JNI_exceptionDescribe();
		JNI_exceptionClear();
		ereport(ERROR, (
			errcode(ERRCODE_INTERNAL_ERROR),
			errmsg("Unable to initialize java session")));
	}
}

static void checkIntTimeType(void)
{
	const char* idt = PG_GETCONFIGOPTION("integer_datetimes");

	integerDateTimes = (strcmp(idt, "on") == 0);
	elog(DEBUG2, integerDateTimes ? "Using integer_datetimes" : "Not using integer_datetimes");
}

static char *get_jni_errmsg(jint jnicode)
{
	switch (jnicode)
	{
		case -1:        return "unknown error";
		case -2:        return "thread detached from the VM";
		case -3:        return "JNI version error";
		case -4:        return "not enough memory";
		case -5:        return "VM already created";
		case -6:        return "invalid arguments";
		default:        return "unknown error";
	}
}

static jint initializeJavaVM(JVMOptList *optList)
{
	jint jstat;
	JavaVMInitArgs vm_args;

	if(pljava_debug)
	{
		elog(INFO, "Backend pid = %d. Attach the debugger and set pljava_debug to false to continue", getpid());
		while(pljava_debug)
			pg_usleep(1000000L);
	}

	vm_args.nOptions = optList->size;
	vm_args.options  = optList->options;
	vm_args.version  = JNI_VERSION_1_4;
	vm_args.ignoreUnrecognized = JNI_FALSE;

	elog(DEBUG2, "creating Java virtual machine");

	jstat = JNI_createVM(&s_javaVM, &vm_args);

	if(jstat == JNI_OK && JNI_exceptionCheck())
	{
		JNI_exceptionDescribe();
		JNI_exceptionClear();
		jstat = JNI_ERR;
	}
	JVMOptList_delete(optList);

	return jstat;
}

static Datum internalCallHandler(bool trusted, PG_FUNCTION_ARGS);

/*
* this is for backward compatibility with 4.x versions as 
* we have pljavau_call_handler in the pg_pltemplate
*/

extern PLJAVADLLEXPORT Datum pljavau_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pljavau_call_handler);
Datum pljavau_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(false, fcinfo);
}

extern PLJAVADLLEXPORT Datum pljava_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pljava_call_handler);

/*
 * This is the entry point for all trusted calls.
 */
Datum pljava_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(true, fcinfo);
}


extern PLJAVADLLEXPORT Datum javau_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(javau_call_handler);

/*
 * This is the entry point for all untrusted calls.
 */
Datum javau_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(false, fcinfo);
}

extern PLJAVADLLEXPORT Datum java_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(java_call_handler);

/*
 * This is the entry point for all trusted calls.
 */
Datum java_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(true, fcinfo);
}

static Datum internalCallHandler(bool trusted, PG_FUNCTION_ARGS)
{
	Invocation ctx;
	Datum retval = 0;

#ifdef USE_PLJAVA_SIGHANDLERS
	/*
	 * Substitute SIGUSR1 handler CatchupInterruptHandler with new local
	 * handler PLCatchupInterruptHandler, which will strictly let main
	 * thread of GPDB to process the signal rather than any other threads.
	 *
	 * Resolving possible lwlock releasing crash caused by SIGUSR1.
	 * MPP-23735
	 */
	if(!s_handlerSubstituted)
	{
		s_mainThreadIdForHandler = pthread_self();
		s_oldHandlerFunc = pqsignal(SIGUSR1, PLCatchupInterruptHandler);
		s_handlerSubstituted = true;
	}
#endif

	/*
	 * Just in case it could be helpful in offering diagnostics later, hang
	 * on to an Oid that is known to refer to PL/Java (because it got here).
	 * It's cheap, and can be followed back to the right language and
	 * handler function entries later if needed.
	 */
	*(trusted ? &pljavaTrustedOid : &pljavaUntrustedOid)
		= fcinfo->flinfo->fn_oid;
	if ( IS_COMPLETE != initstage )
	{
		initsequencer( initstage, false);

		/* Force initial setting
 		 */
		s_currentTrust = !trusted;
	}

	Invocation_pushInvocation(&ctx, trusted);
	PG_TRY();
	{
		Function function = Function_getFunction(fcinfo);
		if(CALLED_AS_TRIGGER(fcinfo))
		{
			/* Called as a trigger procedure
			 */
			retval = Function_invokeTrigger(function, fcinfo);
		}
		else
		{
			/* Called as a function
			 */
			retval = Function_invoke(function, fcinfo);
		}
		Invocation_popInvocation(false);
	}
	PG_CATCH();
	{
		Invocation_popInvocation(true);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return retval;
}

/****************************************
 * JNI methods
 ****************************************/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
	return JNI_VERSION_1_4;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _getConfigOption
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
JNICALL Java_org_postgresql_pljava_internal_Backend__1getConfigOption(JNIEnv* env, jclass cls, jstring jkey)
{
	jstring result = 0;
	
	BEGIN_NATIVE
	char* key = String_createNTS(jkey);
	if(key != 0)
	{
		PG_TRY();
		{
			const char *value = PG_GETCONFIGOPTION(key);
			pfree(key);
			if(value != 0)
				result = String_createJavaStringFromNTS(value);
		}
		PG_CATCH();
		{
			Exception_throw_ERROR("GetConfigOption");
		}
		PG_END_TRY();
	}
	END_NATIVE
	return result;
}


/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _getStatementCacheSize
 * Signature: ()I
 */
JNIEXPORT jint JNICALL
Java_org_postgresql_pljava_internal_Backend__1getStatementCacheSize(JNIEnv* env, jclass cls)
{
	return statementCacheSize;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _log
 * Signature: (ILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
JNICALL Java_org_postgresql_pljava_internal_Backend__1log(JNIEnv* env, jclass cls, jint logLevel, jstring jstr)
{
	BEGIN_NATIVE_NO_ERRCHECK
	char* str = String_createNTS(jstr);
	if(str != 0)
	{
		PG_TRY();
		{
			elog(logLevel, "%s", str);
			pfree(str);
		}
		PG_CATCH();
		{
			Exception_throw_ERROR("ereport");
		}
		PG_END_TRY();
	}
	END_NATIVE
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    isCallingJava
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend_isCallingJava(JNIEnv* env, jclass cls)
{
	return JNI_isCallingJava();
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    isReleaseLingeringSavepoints
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend_isReleaseLingeringSavepoints(JNIEnv* env, jclass cls)
{
	return pljavaReleaseLingeringSavepoints ? JNI_TRUE : JNI_FALSE;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _clearFunctionCache
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_postgresql_pljava_internal_Backend__1clearFunctionCache(JNIEnv* env, jclass cls)
{
	BEGIN_NATIVE_NO_ERRCHECK
	Function_clearFunctionCache();
	END_NATIVE
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _isCreatingExtension
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend__1isCreatingExtension(JNIEnv *env, jclass cls)
{
	bool inExtension = false;
	pljavaCheckExtension( &inExtension);
	return inExtension ? JNI_TRUE : JNI_FALSE;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _getLibraryPath
 * Signature: ()Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
JNICALL Java_org_postgresql_pljava_internal_Backend__1getLibraryPath(JNIEnv* env, jclass cls)
{
	jstring result = 0;

	BEGIN_NATIVE
	PG_TRY();
    {
		result = String_createJavaStringFromNTS(pkglib_path);
	}
	PG_CATCH();
	{
		Exception_throw_ERROR("GetLibraryPath");
	}
	PG_END_TRY();
	END_NATIVE
	return result;
}
