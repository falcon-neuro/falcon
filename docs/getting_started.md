# Getting Started {#mainpage}

Falcon is a modular framework for soft real-time signal processing, originally designed for short-latency closed-loop neural applications. It runs on a graph-based architecture where processors are composed into a directed graph to accomplish tasks like ripple detection, neural decoding, and low-latency feedback.

# ⚡ Quick installation (Debian)

To install Falcon on Debian Linux, simply run the following command in your terminal:

```bash
wget -qO- https://raw.githubusercontent.com/falcon-neuro/falcon/refs/heads/develop/install.sh | bash
```

## Core Workflows

1. **Architecture:** Define your directed processing graphs. Core execution is managed by the framework, while individual processors live as external extensions.
2. **Development:** Build custom processors and data types using C++ and CMake. You can use the boilerplate layout located inside the extensions folder.
3. **Execution:** Compose your processors into a topology graph and pass it to the backend server.

## Usage

TODO: Explain usage of Falcon 2