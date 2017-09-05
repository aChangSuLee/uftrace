/*
 * Python script binding for function entry and exit
 *
 * Copyright (C) 2017, LG Electronics, Honggyu Kim <hong.gyu.kim@lge.com>
 *
 * Released under the GPL v2.
 */

#ifdef HAVE_LIBPYTHON2

#include <dlfcn.h>
#include "utils/symbol.h"
#include "utils/fstack.h"
#include "utils/script.h"
#include "utils/script-python.h"

/* python library name, it only supports python 2.7 as of now */
static const char *libpython = "libpython2.7.so";

/* python library handle returned by dlopen() */
static void *python_handle;

static PyAPI_FUNC(void) (*__Py_Initialize)(void);
static PyAPI_FUNC(void) (*__PySys_SetPath)(char *);
static PyAPI_FUNC(PyObject *) (*__PyImport_Import)(PyObject *name);

static PyAPI_FUNC(PyObject *) (*__PyErr_Occurred)(void);
static PyAPI_FUNC(void) (*__PyErr_Print)(void);

static PyAPI_FUNC(PyObject *) (*__PyObject_GetAttrString)(PyObject *, const char *);
static PyAPI_FUNC(int) (*__PyCallable_Check)(PyObject *);
static PyAPI_FUNC(PyObject *) (*__PyObject_CallObject)(PyObject *callable_object, PyObject *args);

static PyAPI_FUNC(PyObject *) (*__PyString_FromString)(const char *);
static PyAPI_FUNC(PyObject *) (*__PyInt_FromLong)(long);
static PyAPI_FUNC(PyObject *) (*__PyLong_FromLong)(long);
static PyAPI_FUNC(PyObject *) (*__PyLong_FromUnsignedLongLong)(unsigned PY_LONG_LONG);

static PyAPI_FUNC(char *) (*__PyString_AsString)(PyObject *);
static PyAPI_FUNC(long) (*__PyLong_AsLong)(PyObject *);

static PyAPI_FUNC(PyObject *) (*__PyTuple_New)(Py_ssize_t size);
static PyAPI_FUNC(int) (*__PyTuple_SetItem)(PyObject *, Py_ssize_t, PyObject *);

static PyAPI_FUNC(PyObject *) (*__PyDict_New)(void);
static PyAPI_FUNC(int) (*__PyDict_SetItem)(PyObject *mp, PyObject *key, PyObject *item);
static PyAPI_FUNC(int) (*__PyDict_SetItemString)(PyObject *dp, const char *key, PyObject *item);
static PyAPI_FUNC(PyObject *) (*__PyDict_GetItem)(PyObject *mp, PyObject *key);

static PyObject *pName, *pModule, *pFuncEntry, *pFuncExit, *pFuncEnd;

extern struct symtabs symtabs;

enum py_context_idx {
	PY_CTX_TID = 0,
	PY_CTX_DEPTH,
	PY_CTX_TIMESTAMP,
	PY_CTX_DURATION,
	PY_CTX_ADDRESS,
	PY_CTX_SYMNAME,
};

/* The order has to be aligned with enum py_args above. */
static const char *py_context_table[] = {
	"tid",
	"depth",
	"timestamp",
	"duration",
	"address",
	"symname",
};

#define INIT_PY_API_FUNC(func) \
	do { \
		__##func = dlsym(python_handle, #func); \
		if (!__##func) { \
			pr_err("dlsym for \"" #func "\" is failed!\n"); \
			return -1; \
		} \
	} while (0)

static char *remove_py_suffix(char *py_name)
{
	char *ext = strrchr(py_name, '.');

	if (!ext)
		return NULL;

	*ext = '\0';
	return py_name;
}

static int set_python_path(char *py_pathname)
{
	char py_sysdir[PATH_MAX];
	char *old_sysdir = getenv("PYTHONPATH");
	char *new_sysdir = NULL;

	if (absolute_dirname(py_pathname, py_sysdir) == NULL)
		return -1;

	if (old_sysdir)
		xasprintf(&new_sysdir, "%s:%s", old_sysdir, py_sysdir);
	else
		new_sysdir = xstrdup(py_sysdir);

	setenv("PYTHONPATH", new_sysdir, 1);
	free(new_sysdir);

	return 0;
}

/* Import python module that is given by -p option. */
static int import_python_module(char *py_pathname)
{
	char *py_basename = basename(py_pathname);
	remove_py_suffix(py_basename);

	pName = __PyString_FromString(py_basename);
	pModule = __PyImport_Import(pName);
	if (pModule == NULL) {
		__PyErr_Print();
		pr_warn("%s.py cannot be imported!\n", py_pathname);
		return -1;
	}

	return 0;
}

union python_val {
	long			l;
	unsigned long long	ull;
	char			*s;
};

static void python_insert_tuple(PyObject *tuple, char type, int idx,
				union python_val val)
{
	PyObject *obj;

	switch (type) {
	case 'l':
		obj = __PyInt_FromLong(val.l);
		break;
	case 'U':
		obj = __PyLong_FromUnsignedLongLong(val.ull);
		break;
	case 's':
		obj = __PyString_FromString(val.s);
		break;
	default:
		pr_warn("unsupported data type was added to tuple\n");
		obj = NULL;
		break;
	}

	__PyTuple_SetItem(tuple, idx, obj);
}

static void python_insert_dict(PyObject *dict, char type, const char *key,
			       union python_val val)
{
	PyObject *obj;

	switch (type) {
	case 'l':
		obj = __PyInt_FromLong(val.l);
		break;
	case 'U':
		obj = __PyLong_FromUnsignedLongLong(val.ull);
		break;
	case 's':
		obj = __PyString_FromString(val.s);
		break;
	default:
		pr_warn("unsupported data type was added to dict\n");
		obj = NULL;
		break;
	}

	__PyDict_SetItemString(dict, key, obj);
	Py_XDECREF(obj);
}

static void insert_tuple_long(PyObject *tuple, int idx, long v)
{
	union python_val val = { .l = v, };
	python_insert_tuple(tuple, 'l', idx, val);
}

static void insert_tuple_ull(PyObject *tuple, int idx, unsigned long long v)
{
	union python_val val = { .ull = v, };
	python_insert_tuple(tuple, 'U', idx, val);
}

static void insert_tuple_string(PyObject *tuple, int idx, char *v)
{
	union python_val val = { .s = v, };
	python_insert_tuple(tuple, 's', idx, val);
}

static void insert_dict_long(PyObject *dict, const char *key, long v)
{
	union python_val val = { .l = v, };
	python_insert_dict(dict, 'l', key, val);
}

static void insert_dict_ull(PyObject *dict, const char *key, unsigned long long v)
{
	union python_val val = { .ull = v, };
	python_insert_dict(dict, 'U', key, val);
}

static void insert_dict_string(PyObject *dict, const char *key, char *v)
{
	union python_val val = { .s = v, };
	python_insert_dict(dict, 's', key, val);
}

#define PYCTX(_item)  py_context_table[PY_CTX_##_item]

static void setup_common_context(PyObject **pDict, struct script_context *sc_ctx)
{
	insert_dict_long(*pDict, PYCTX(TID), sc_ctx->tid);
	insert_dict_long(*pDict, PYCTX(DEPTH), sc_ctx->depth);
	insert_dict_ull(*pDict, PYCTX(TIMESTAMP), sc_ctx->timestamp);
	insert_dict_long(*pDict, PYCTX(ADDRESS), sc_ctx->address);
	insert_dict_string(*pDict, PYCTX(SYMNAME), sc_ctx->symname);
}

int python_uftrace_entry(struct script_context *sc_ctx)
{
	if (unlikely(!pFuncEntry))
		return -1;

	/* Entire arguments are passed into a single dictionary. */
	PyObject *pDict = __PyDict_New();

	/* Setup common info in both entry and exit into a dictionary */
	setup_common_context(&pDict, sc_ctx);

	/* Python function arguments must be passed in a tuple. */
	PyObject *pythonContext = __PyTuple_New(1);
	__PyTuple_SetItem(pythonContext, 0, pDict);

	/* Call python function "uftrace_entry". */
	__PyObject_CallObject(pFuncEntry, pythonContext);

	/* Free PyTuple. */
	Py_XDECREF(pythonContext);

	return 0;
}

int python_uftrace_exit(struct script_context *sc_ctx)
{
	if (unlikely(!pFuncExit))
		return -1;

	/* Entire arguments are passed into a single dictionary. */
	PyObject *pDict = __PyDict_New();

	/* Setup common info in both entry and exit into a dictionary */
	setup_common_context(&pDict, sc_ctx);

	/* Add time duration info */
	insert_dict_ull(pDict, PYCTX(DURATION), sc_ctx->duration);

	/* Python function arguments must be passed in a tuple. */
	PyObject *pythonContext = __PyTuple_New(1);
	__PyTuple_SetItem(pythonContext, 0, pDict);

	/* Call python function "uftrace_exit". */
	__PyObject_CallObject(pFuncExit, pythonContext);

	/* Free PyTuple. */
	Py_XDECREF(pythonContext);

	return 0;
}

int python_uftrace_end(void)
{
	if (unlikely(!pFuncEnd))
		return -1;

	/* Call python function "uftrace_end". */
	__PyObject_CallObject(pFuncEnd, NULL);

	return 0;
}

int script_init_for_python(char *py_pathname)
{
	pr_dbg("initialize python scripting engine for %s\n", py_pathname);

	/* Bind script_uftrace functions to python's. */
	script_uftrace_entry = python_uftrace_entry;
	script_uftrace_exit = python_uftrace_exit;
	script_uftrace_end = python_uftrace_end;

	python_handle = dlopen(libpython, RTLD_LAZY | RTLD_GLOBAL);
	if (!python_handle) {
		pr_warn("%s cannot be loaded!\n", libpython);
		return -1;
	}

	INIT_PY_API_FUNC(Py_Initialize);
	INIT_PY_API_FUNC(PySys_SetPath);
	INIT_PY_API_FUNC(PyImport_Import);

	INIT_PY_API_FUNC(PyErr_Occurred);
	INIT_PY_API_FUNC(PyErr_Print);

	INIT_PY_API_FUNC(PyObject_GetAttrString);
	INIT_PY_API_FUNC(PyCallable_Check);
	INIT_PY_API_FUNC(PyObject_CallObject);

	INIT_PY_API_FUNC(PyString_FromString);
	INIT_PY_API_FUNC(PyInt_FromLong);
	INIT_PY_API_FUNC(PyLong_FromLong);
	INIT_PY_API_FUNC(PyLong_FromUnsignedLongLong);

	INIT_PY_API_FUNC(PyString_AsString);
	INIT_PY_API_FUNC(PyLong_AsLong);

	INIT_PY_API_FUNC(PyTuple_New);
	INIT_PY_API_FUNC(PyTuple_SetItem);

	INIT_PY_API_FUNC(PyDict_New);
	INIT_PY_API_FUNC(PyDict_SetItem);
	INIT_PY_API_FUNC(PyDict_SetItemString);
	INIT_PY_API_FUNC(PyDict_GetItem);

	set_python_path(py_pathname);

	__Py_Initialize();

	/* Import python module that is passed by -p option. */
	if (import_python_module(py_pathname) < 0)
		return -1;


	/* Call python function "uftrace_begin" immediately if possible. */
	PyObject *pFuncBegin = __PyObject_GetAttrString(pModule, "uftrace_begin");
	if (pFuncBegin && __PyCallable_Check(pFuncBegin))
		__PyObject_CallObject(pFuncBegin, NULL);

	pFuncEntry = __PyObject_GetAttrString(pModule, "uftrace_entry");
	if (!pFuncEntry || !__PyCallable_Check(pFuncEntry)) {
		if (__PyErr_Occurred())
			__PyErr_Print();
		pr_dbg("uftrace_entry is not callable!\n");
		pFuncEntry = NULL;
	}
	pFuncExit = __PyObject_GetAttrString(pModule, "uftrace_exit");
	if (!pFuncExit || !__PyCallable_Check(pFuncExit)) {
		if (__PyErr_Occurred())
			__PyErr_Print();
		pr_dbg("uftrace_exit is not callable!\n");
		pFuncExit = NULL;
	}
	pFuncEnd = __PyObject_GetAttrString(pModule, "uftrace_end");
	if (!pFuncEnd || !__PyCallable_Check(pFuncEnd)) {
		pr_dbg("uftrace_end is not callable!\n");
		pFuncEnd = NULL;
	}

	pr_dbg("python initialization finished\n");

	return 0;
}

#endif /* !HAVE_LIBPYTHON2 */
