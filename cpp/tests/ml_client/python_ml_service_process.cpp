#include "python_ml_service_process.hpp"

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <httplib.h>

#include <chrono>
#include <stdexcept>
#include <thread>

// ML_SERVICE_DIR is injected by cpp/tests/ml_client/CMakeLists.txt as an
// absolute path — see that file for why (mirrors QUICKFIX_FIX42_SPEC_PATH's
// pattern in cpp/tests/fix/).
#ifndef ML_SERVICE_DIR
#error "ML_SERVICE_DIR must be defined by CMake"
#endif

namespace tse::ml_client::testutil {

PythonMlServiceProcess::PythonMlServiceProcess(int port, int artificial_delay_ms) : port_(port) {
    const std::string ml_service_dir = ML_SERVICE_DIR;
    const std::string venv_python = ml_service_dir + "/.venv/bin/python3";
    const std::string port_str = std::to_string(port);

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("PythonMlServiceProcess: fork() failed");
    }

    if (pid == 0) {
        // Child process from here on -- never returns to the caller.
        if (artificial_delay_ms > 0) {
            setenv("ML_SERVICE_ARTIFICIAL_DELAY_MS", std::to_string(artificial_delay_ms).c_str(), 1);
        }
        if (chdir(ml_service_dir.c_str()) != 0) {
            _exit(127);
        }
        // Redirect stdout/stderr to /dev/null -- uvicorn's startup banner
        // isn't needed and would otherwise interleave with gtest's own
        // captured output.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl(venv_python.c_str(), venv_python.c_str(), "-m", "uvicorn", "app.main:app", "--host", "127.0.0.1",
              "--port", port_str.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // execl only returns on failure (e.g. venv_python doesn't exist)
    }

    pid_ = pid;
}

PythonMlServiceProcess::~PythonMlServiceProcess() { kill_now(); }

bool PythonMlServiceProcess::wait_until_ready(std::chrono::milliseconds timeout) {
    httplib::Client client("http://127.0.0.1:" + std::to_string(port_));
    client.set_connection_timeout(0, 200'000);
    client.set_read_timeout(0, 200'000);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!is_running()) return false;  // child exited before ever becoming ready (e.g. missing venv)
        httplib::Result res = client.Get("/health");
        if (res && res->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void PythonMlServiceProcess::kill_now() {
    if (pid_ > 0 && !killed_) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        killed_ = true;
    }
}

bool PythonMlServiceProcess::is_running() const {
    if (pid_ <= 0 || killed_) return false;
    return ::kill(pid_, 0) == 0;  // signal 0: existence/permission check only, doesn't actually signal
}

}  // namespace tse::ml_client::testutil
