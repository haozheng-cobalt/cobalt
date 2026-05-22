# Guide: Choosing Search Modes for Feature Disable Difficulty Analysis

This guide helps Cobalt developers determine which **search mode** and **value** to pass to `analyze_feature_disable_difficulty.py` when evaluating a high-level concept (like "Ad Auction", "WebRTC", or "Audio Worklet").

---

## Search Mode Selection Flowchart

```mermaid
graph TD
    Start[Want to analyze a concept?] --> PathCheck{Does the feature have its own folder?}

    PathCheck -->|Yes| Path[Use --path "path/to/folder"]
    PathCheck -->|No| MetaCheck{Does the folder have a DIR_METADATA file?}

    MetaCheck -->|Yes| Component[Use --component "Blink>ComponentName"]
    MetaCheck -->|No| SymCheck{Do you know a main class or function name?}

    SymCheck -->|Yes| Symbol[Use --symbol "ClassName"]
    SymCheck -->|No| Namespace[Use C++ namespace: --namespace "blink::ns"]
```

---

## Detailed Strategies

### 1. By Path (`--path`) — *Recommended*
Use this when the feature's source code is isolated inside its own folder. This is the fastest and most direct query.

* **When to use**: When the concept corresponds to a directory in the repository.
* **How to find the value**:
  Run a file system search for folders matching your concept's name:
  ```bash
  find third_party/blink/renderer/modules -type d -iname "*auction*"
  ```
  *Example Output*: `third_party/blink/renderer/modules/ad_auction`
* **Example Command**:
  ```bash
  python3 cobalt/tools/binary_size/analyze_feature_disable_difficulty.py \
    --path "third_party/blink/renderer/modules/ad_auction" \
    --size_file cobalt27_1.size
  ```

### 2. By Component (`--component`) — *Architectural*
Chromium features are mapped to official ownership components (e.g., `Blink>Storage`, `Blink>WebRTC>PeerConnection`). This queries all symbols matching that component's metadata.

* **When to use**: When a feature's source code is spread across multiple directories (e.g., some files in `renderer/core`, some in `renderer/modules`).
* **How to find the value**:
  Look at the `DIR_METADATA` file in the feature's folder:
  ```bash
  cat third_party/blink/renderer/modules/ad_auction/DIR_METADATA
  ```
  Look for the `monorail` component:
  ```text
  monorail {
    component: "Blink>AdAuction"
  }
  ```
* **Example Command**:
  ```bash
  python3 cobalt/tools/binary_size/analyze_feature_disable_difficulty.py \
    --component "Blink>AdAuction" \
    --size_file cobalt27_1.size
  ```

### 3. By Symbol (`--symbol`) — *Surgical*
Queries the SuperSize database for one specific C++ class, namespace, or function.

* **When to use**: When the feature code is compiled into shared files rather than isolated folders, and you want to target a specific class.
* **How to find the value**:
  Search the codebase for class declarations containing your keyword:
  ```bash
  git grep "class.*Auction"
  ```
  *Example Output*: `class NavigatorAuction : ...`
* **Example Command**:
  ```bash
  python3 cobalt/tools/binary_size/analyze_feature_disable_difficulty.py \
    --symbol "NavigatorAuction" \
    --size_file cobalt27_1.size
  ```

### 4. By Namespace (`--namespace`) — *C++ Scope*
Queries all symbols wrapped inside a specific C++ namespace.

* **When to use**: When files are mixed inside a shared target, but the C++ code is encapsulated inside a clear namespace block.
* **How to find the value**:
  Look inside any of the feature's C++ header or source files:
  ```cpp
  namespace blink {
  namespace ad_auction {
  ...
  }
  }
  ```
* **Example Command**:
  ```bash
  python3 cobalt/tools/binary_size/analyze_feature_disable_difficulty.py \
    --namespace "blink::ad_auction" \
    --size_file cobalt27_1.size
  ```

---

## Algorithmic Limitations & Gaps of the Analysis Tool

When planning a feature removal or analyzing the difficulty output, developers must be aware of **four critical limitations** of the `analyze_feature_disable_difficulty.py` script. Because the tool relies strictly on static C++ `#include` parsing and production-compiled SuperSize C++ symbols, it is blind to several key Chromium/Blink pipelines:

### 1. Omission of the Web IDL & V8 Bindings Pipeline
Web IDL files (`.idl`) describe the APIs exposed to JavaScript. Blink processes these at build time to auto-generate massive C++ binding translation units.

* **What the script misses**: Since IDL files and `.gni` lists are pre-compilation files, they do not generate SuperSize database records. The script will **never** instruct you to modify bindings paths.
* **The Blocker**: If you only follow the script's C++ instructions, V8 binding code generators will still try to compile bindings for feature classes that no longer exist, causing instant compilation crashes.
* **Developer Fix**: You must manually open `bindings/idl_in_modules.gni` and bindings lists to filter out the feature's IDL assets.

### 2. Exclusion of Unit Test Suites and Mocking Code
Test-runners (e.g., `blink_unittests`) compile code separately from production executables (`libchrobalt.so`).

* **What the script misses**: The script maps reachability only from the production root target (e.g., `//cobalt:gn_all`). It is completely blind to files suffixed with `_test.cc` or `_unittest.cc`.
* **The Blocker**: When you gate or subtract the production targets, the unit tests compiled in separate test targets will break in the CI pipeline because they still try to compile feature test files.
* **Developer Fix**: You must manually search for test-only files under the feature's path and wrap or subtract them inside the `source_set("unit_tests")` target blocks in parent directories.

### 3. Blindness to Transitive GN Graph Dependencies
Some parent targets include the feature under their `deps` block simply to inherit public compilation configs or library linkages, even if they don't directly include the feature's headers in their C++ source code.

* **What the script misses**: The include scanner only flags parent targets whose `.cc` source files contain a literal `#include` line to the feature.
* **The Blocker**: Target B might depend on feature A for transitive configs. If you remove A, target B's compilation flags might break, but the script will report $0$ integration points for target B, leaving you with broken, untracked GN graphs.
* **Developer Fix**: Run `gn refs out/<dir> //path/to/feature` manually to identify all parent targets that list the feature in their `deps` or `public_deps`, even if they are not highlighted by the script.

### 4. Blindness to Mojo IPC Interfaces & Dependency Injection
Chromium decouples code heavily via Mojo IPC interfaces and runtime dependency injection (like registries or abstract factories).

* **What the script misses**: Renderer files and browser controllers communicate using auto-generated Mojo interfaces (e.g. `#include "third_party/blink/public/mojom/interest_group/ad_auction_service.mojom-blink.h"`). They do **not** directly include the C++ implementation class header (e.g., `ad_auction_service_impl.h`). Because the script only matches production implementation headers, it reports **$0$ includes/integration points** for the Mojo caller.
* **The Blocker**: The code will compile cleanly, but at runtime, when the page tries to connect to the Mojo interface, the connection will fail silently or cause a browser crash because the binding registrar was never safely gated.
* **Developer Fix**: Always check for `.mojom` files inside your feature directory. Locate where these interfaces are registered in Mojo binder maps (e.g., `browser_interface_binders.cc`) and manually wrap those bindings.
