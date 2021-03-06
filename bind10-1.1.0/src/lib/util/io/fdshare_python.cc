// Copyright (C) 2010  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>

#include <config.h>

#include "fd_share.h"


static PyObject*
fdshare_recv_fd(PyObject*, PyObject* args) {
    int sock, fd;
    if (!PyArg_ParseTuple(args, "i", &sock)) {
        return (NULL);
    }
    fd = isc::util::io::recv_fd(sock);
    return (Py_BuildValue("i", fd));
}

static PyObject*
fdshare_send_fd(PyObject*, PyObject* args) {
    int sock, fd, result;
    if (!PyArg_ParseTuple(args, "ii", &sock, &fd)) {
        return (NULL);
    }
    result = isc::util::io::send_fd(sock, fd);
    return (Py_BuildValue("i", result));
}

static PyMethodDef fdshare_Methods[] = {
    {"send_fd",  fdshare_send_fd, METH_VARARGS, ""},
    {"recv_fd",  fdshare_recv_fd, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


static PyModuleDef bind10_fdshare_python = {
    { PyObject_HEAD_INIT(NULL) NULL, 0, NULL},
    "bind10_fdshare",
    "Python bindings for fdshare",
    -1,
    fdshare_Methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit_libutil_io_python(void) {
    PyObject *mod = PyModule_Create(&bind10_fdshare_python);
    if (mod == NULL) {
        return (NULL);
    }

    PyObject* FD_SYSTEM_ERROR = Py_BuildValue("i",
                                              isc::util::io::FD_SYSTEM_ERROR);
    if (FD_SYSTEM_ERROR == NULL) {
        Py_XDECREF(mod);
        return (NULL);
    }
    int ret = PyModule_AddObject(mod, "FD_SYSTEM_ERROR", FD_SYSTEM_ERROR);
    if (ret == -1) {
        Py_XDECREF(FD_SYSTEM_ERROR);
        Py_XDECREF(mod);
        return (NULL);
    }

    PyObject* FD_OTHER_ERROR = Py_BuildValue("i",
                                             isc::util::io::FD_OTHER_ERROR);
    if (FD_OTHER_ERROR == NULL) {
        Py_XDECREF(mod);
        return (NULL);
    }
    ret = PyModule_AddObject(mod, "FD_OTHER_ERROR", FD_OTHER_ERROR);
    if (-1 == ret) {
        Py_XDECREF(FD_OTHER_ERROR);
        Py_XDECREF(mod);
        return (NULL);
    }

    return (mod);
}

