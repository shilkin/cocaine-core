#include "cocaine/downloads.hpp"
#include "cocaine/helpers/uri.hpp"

#include "python+raw.hpp"
#include "python+wsgi.hpp"
#include "python+json.hpp"

using namespace cocaine::plugin;

source_t* raw_python_t::create(const std::string& args) {
    return new raw_python_t(args);
}

raw_python_t::raw_python_t(const std::string& args):
    m_module(NULL)
{
    if(args.empty()) {
        throw std::runtime_error("no code location has been specified");
    }

    helpers::uri_t uri(args);
    helpers::download_t app(helpers::download(uri));   

    // Acquire the interpreter state
    thread_state_t state;

    // NOTE: Prepend the current application cache location to the sys.path,
    // so that it could import different stuff from there
    PyObject* paths = PySys_GetObject("path");

    if(PyList_Check(paths)) {
        // XXX: Does it steal the reference or not?
        PyList_Insert(paths, 0, 
            PyString_FromString(app.path().string().c_str()));
        compile(app.path().string(), app);
    } else {
        throw std::runtime_error("'sys.path' is not a list object");
    }
}

void raw_python_t::invoke(
    callback_fn_t callback,
    const std::string& method,
    const void* request,
    size_t size) 
{
    thread_state_t state;
    object_t args(NULL);

    if(request && size) {
#if PY_VERSION_HEX > 0x02070000
        Py_buffer* buffer = static_cast<Py_buffer*>(malloc(sizeof(Py_buffer)));

        buffer->buf = const_cast<void*>(request);
        buffer->len = size;
        buffer->readonly = true;
        buffer->format = NULL;
        buffer->ndim = 0;
        buffer->shape = NULL;
        buffer->strides = NULL;
        buffer->suboffsets = NULL;

        object_t view(PyMemoryView_FromBuffer(buffer));
#else
        object_t view(PyBuffer_FromMemory(const_cast<void*>(request), size));
#endif

        args = PyTuple_Pack(1, *view);
    } else {
        args = PyTuple_New(0);
    }

    object_t object(PyObject_GetAttrString(m_module, method.c_str()));
    
    if(PyErr_Occurred()) {
        exception();
    }

    if(!PyCallable_Check(object)) {
        throw std::runtime_error("'" + method + "' is not callable");
    }

    if(PyType_Check(object)) {
        if(PyType_Ready(reinterpret_cast<PyTypeObject*>(*object)) != 0) {
            exception();
        }
    }

    object_t result(PyObject_Call(object, args, NULL));

    if(PyErr_Occurred()) {
        exception();
    } else if(result.valid()) {
        respond(callback, result);
    }
}

void raw_python_t::respond(callback_fn_t callback, object_t& result) {
    if(PyString_Check(result)) {
        throw std::runtime_error("the result must be an iterable");
    }

    object_t iterator(PyObject_GetIter(result));

    if(iterator.valid()) {
        object_t item(NULL);

        while(true) {
            item = PyIter_Next(iterator);

            if(PyErr_Occurred()) {
                exception();
            } else if(!item.valid()) {
                break;
            }
        
#if PY_VERSION_HEX > 0x02060000
            if(PyObject_CheckBuffer(item)) {
                boost::shared_ptr<Py_buffer> buffer(
                    static_cast<Py_buffer*>(malloc(sizeof(Py_buffer))),
                    free);

                if(PyObject_GetBuffer(item, buffer.get(), PyBUF_SIMPLE) == 0) {
                    Py_BEGIN_ALLOW_THREADS
                        callback(buffer->buf, buffer->len);
                    Py_END_ALLOW_THREADS
                    
                    PyBuffer_Release(buffer.get());
                } else {
                    throw std::runtime_error("unable to serialize the result");
                }
            }
#else
            if(PyString_Check(item)) {
                callback(PyString_AsString(item), PyString_Size(item));
            } else {
                throw std::runtime_error("unable to serialize the result");
            }
#endif
        }
    } else {
        exception();
    }
}

void raw_python_t::exception() {
    object_t type(NULL), object(NULL), traceback(NULL);
    
    PyErr_Fetch(&type, &object, &traceback);
    object_t message(PyObject_Str(object));
    
    throw std::runtime_error(PyString_AsString(message));
}

void raw_python_t::compile(const std::string& path, const std::string& code) {
    object_t bytecode(Py_CompileString(
        code.c_str(),
        path.c_str(),
        Py_file_input));

    if(PyErr_Occurred()) {
        exception();
    }

    m_module = PyImport_ExecCodeModule(
        const_cast<char*>(unique_id_t().id().c_str()),
        bytecode);
    
    if(PyErr_Occurred()) {
        exception();
    }
}

static const source_info_t plugin_info[] = {
    { "python+raw", &raw_python_t::create },
//  { "python+wsgi", &wsgi_python_t::create },
    { "python+json", &json_python_t::create },
    { NULL, NULL }
};

extern "C" {
    PyThreadState* g_state = NULL;

    void save() {
        g_state = PyEval_SaveThread();
    }

    void restore() {
        PyEval_RestoreThread(g_state);
    }

    const source_info_t* initialize() {
        // Initialize the Python subsystem
        Py_InitializeEx(0);

        // Initialize the GIL
        PyEval_InitThreads();
        save();

        // NOTE: In case of a fork, restore the main thread state and acquire the GIL,
        // call the python post-fork handler and save the main thread again, releasing the GIL.
        pthread_atfork(NULL, NULL, restore);
        pthread_atfork(NULL, NULL, PyOS_AfterFork);
        pthread_atfork(NULL, NULL, save);

        return plugin_info;
    }

    __attribute__((destructor)) void finalize() {
        PyEval_RestoreThread(g_state);
        Py_Finalize();
    }
}
