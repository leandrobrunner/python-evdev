
/*
 * Python bindings to certain linux input subsystem functions.
 *
 * While everything here can be implemented in pure Python with struct and
 * fcntl.ioctl, imho, it is much more straightforward to do so in C.
 *
 */

#include <Python.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/input.h>

#define MAX_NAME_SIZE 256

extern char*  EV_NAME[EV_CNT];
extern int    EV_TYPE_MAX[EV_CNT];
extern char** EV_TYPE_NAME[EV_CNT];
extern char*  BUS_NAME[];


int test_bit(const char* bitmask, int bit) {
    return bitmask[bit/8] & (1 << (bit % 8));
}


// useful for comparing input events as seen in the extension module
// and as seen in python
static void
print_event(struct input_event *ev) {
    fprintf(stderr, "[so] event: time %ld.%06ld, code %02d, type %02d, val %02d\n",
            ev->time.tv_sec, ev->time.tv_usec,
            ev->code, ev->type, ev->value
    );
}


// Read input event from a device and return a tuple that mimics input_event
static PyObject *
device_read(PyObject *self, PyObject *args)
{
    int fd;
    struct input_event event;

    // get device file descriptor (O_RDONLY|O_NONBLOCK)
    if (PyArg_ParseTuple(args, "i", &fd) < 0)
        return NULL;

    read(fd, &event, sizeof(event));
    int n = read(fd, &event, sizeof(event));

    if (n<0) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyObject* sec  = PyLong_FromLong(event.time.tv_sec);
    PyObject* usec = PyLong_FromLong(event.time.tv_usec);
    PyObject* val  = PyLong_FromLong(event.value);

    return Py_BuildValue("OOhhO", sec, usec, event.type, event.code, val);
}


// Read multiple input events from a device and return a list of tuples
static PyObject *
device_read_many(PyObject *self, PyObject *args)
{
    int fd, i;

    // get device file descriptor (O_RDONLY|O_NONBLOCK)
    int ret = PyArg_ParseTuple(args, "i", &fd);
    if (!ret) return NULL;

    PyObject* event_list = PyList_New(0);
    PyObject* py_input_event = NULL;
    PyObject* sec  = NULL;
    PyObject* usec = NULL;
    PyObject* val  = NULL;

    struct input_event event[64];

    size_t event_size = sizeof(struct input_event);
    size_t nread;

    nread = read(fd, event, event_size*64);

    // Construct a list of event tuples, which we'll make sense of in Python
    for (i = 0 ; i < nread/event_size ; i++) {
        sec  = PyLong_FromLong(event[i].time.tv_sec);
        usec = PyLong_FromLong(event[i].time.tv_usec);
        val  = PyLong_FromLong(event[i].value);

        py_input_event = Py_BuildValue("OOhhO", sec, usec, event[i].type, event[i].code, val);
        PyList_Append(event_list, py_input_event);
    }

    return event_list;
}


// Unpack a single event (this is essentially a struct.unpack(), without having
// to worry about word size.
static PyObject *
event_unpack(PyObject *self, PyObject *args)
{
    struct input_event event;

    const char *data;
    int len;

    int ret = PyArg_ParseTuple(args, "s#", &data, &len);
    if (!ret) return NULL;

    memcpy(&event, data, sizeof(event));

    Py_RETURN_NONE;
}


// Get the event types and event codes that the input device supports
static PyObject *
ioctl_capabilities(PyObject *self, PyObject *args)
{
    int abs_bits[6] = {0};
    int fd, ev_type, ev_code;
    char ev_bits[EV_MAX/8], code_bits[KEY_MAX/8];

    int ret = PyArg_ParseTuple(args, "i", &fd);
    if (!ret) return NULL;

    // @todo: figure out why fd gets zeroed on the ioctls after the
    // refactoring and get rid of this workaround
    const int _fd = fd;

    // Capabilities is a mapping of supported event types to lists of handled
    // evenes e.g: {1: [272, 273, 274, 275], 2: [0, 1, 6, 8]}
    PyObject* capabilities = PyDict_New();
    PyObject* eventcodes = NULL;
    PyObject* capability = NULL;
    PyObject* absdata = NULL;
    PyObject* absitem = NULL;

    memset(&ev_bits, 0, sizeof(ev_bits));

    if (ioctl(_fd, EVIOCGBIT(0, EV_MAX), ev_bits) < 0)
        goto on_err;

    // Build a dictionary of the device's capabilities
    for (ev_type=0 ; ev_type<EV_MAX ; ev_type++) {
        if (test_bit(ev_bits, ev_type)) {
            capability = PyLong_FromLong(ev_type);
            eventcodes = PyList_New(0);

            memset(&code_bits, 0, sizeof(code_bits));
            ioctl(_fd, EVIOCGBIT(ev_type, KEY_MAX), code_bits);
            for (ev_code = 0; ev_code < KEY_MAX; ev_code++) {
                if (test_bit(code_bits, ev_code)) {
                    if (ev_type == EV_ABS) {
                        memset(&abs_bits, 0, sizeof(abs_bits));
                        ioctl(_fd, EVIOCGABS(ev_code), abs_bits);

                        absdata = Py_BuildValue("(iiiii)",   abs_bits[0], abs_bits[1],
                                                abs_bits[2], abs_bits[3], abs_bits[4],
                                                abs_bits[5]);

                        absitem = Py_BuildValue("(OO)", PyLong_FromLong(ev_code), absdata);
                        PyList_Append(eventcodes, absitem);
                    } else {
                        PyList_Append(eventcodes, PyLong_FromLong(ev_code));
                    }

                }
            }

            PyDict_SetItem(capabilities, capability, eventcodes);
        }
    }

    return Py_BuildValue("O", capabilities);

    on_err:
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
}


// An all-in-one function for describing an input device
static PyObject *
ioctl_devinfo(PyObject *self, PyObject *args)
{
    int fd;
    PyObject* capabilities = NULL;

    struct input_id iid;
    char name[MAX_NAME_SIZE];
    char phys[MAX_NAME_SIZE] = {0};

    int ret = PyArg_ParseTuple(args, "i", &fd);
    if (!ret) return NULL;

    memset(&iid,  0, sizeof(iid));

    if (ioctl(fd, EVIOCGID, &iid) < 0)                 goto on_err;
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) goto on_err;

    // Get device capabilities
    capabilities = ioctl_capabilities(self, Py_BuildValue("(i)", fd));

    // Some devices do not have a physical topology associated with them
    ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);

    return Py_BuildValue("hhhhssO", iid.bustype, iid.vendor, iid.product, iid.version,
                         name, phys, capabilities);

    on_err:
        PyErr_SetFromErrno(PyExc_IOError);
        return NULL;
}


static PyMethodDef MethodTable[] = {
    { "unpack",             event_unpack,       METH_VARARGS, "unpack a single input event" },
    { "ioctl_devinfo",      ioctl_devinfo,      METH_VARARGS, "fetch input device info" },
    { "ioctl_capabilities", ioctl_capabilities, METH_VARARGS, "fetch input device capabilities" },
    { "device_read",        device_read,        METH_VARARGS, "read an input event from a device" },
    { "device_read_many",   device_read_many,   METH_VARARGS, "read all available input events from a device" },

    { NULL, NULL, 0, NULL}
};


#define MODULE_NAME "_input"
#define MODULE_HELP "Python bindings to certain linux input subsystem functions"

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    MODULE_NAME,
    MODULE_HELP,
    -1,          /* m_size */
    MethodTable, /* m_methods */
    NULL,        /* m_reload */
    NULL,        /* m_traverse */
    NULL,        /* m_clear */
    NULL,        /* m_free */
};

static PyObject *
moduleinit(void)
{
    PyObject* m = PyModule_Create(&moduledef);
    if (m == NULL) return NULL;
    return m;
}

PyMODINIT_FUNC
PyInit__input(void)
{
    return moduleinit();
}

#else
static PyObject *
moduleinit(void)
{
    PyObject* m = Py_InitModule3(MODULE_NAME, MethodTable, MODULE_HELP);
    if (m == NULL) return NULL;
    return m;
}

PyMODINIT_FUNC
init_input(void)
{
    moduleinit();
}
#endif
