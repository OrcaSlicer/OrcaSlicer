# AI Slicer

An optional, printer-agnostic AI assistant built into OrcaSlicer: register an LLM
gateway (OpenAI, Anthropic, or any OpenAI-compatible endpoint), then **generate 3D
shapes from a text description** or **search the web for a model and import it** — with
the generation grounded in your *real* machine (bed size, nozzle, material, live
temperatures, and a camera frame when available).

The feature is entirely **opt-in**: nothing contacts a network until you configure a
provider and invoke it from the **AI** menu.

## User guide

### 1. Register a provider — *AI ▸ AI Settings…*
- **Provider**: `OpenAI`, `Anthropic`, or `OpenAI-compatible (custom gateway)`.
- **API key**: stored locally (see *Security* below).
- **Gateway URL**: required only for the compatible provider (Ollama, LiteLLM, an
  Azure/OpenAI proxy, …); ignored otherwise.
- **Model**: type it, or click **Fetch models** to list what the key can access.
- **Test connection** verifies the key/endpoint before you save. **OK** persists the
  settings.

### 2. Generate a shape — *AI ▸ Generate 3D Shape…* → *Generate shape* tab
Describe the object (“a 40 mm hex knob with a 6 mm shaft hole”) and press **Generate**.
The assistant receives your machine context, returns a shape, and it is dropped on the
plate. If the result exceeds the build volume it is still added, with a warning, so you
can rescale.

### 3. Find a model on the web — *…* → *Search & import* tab
Enter a query, **Search**, pick a result, and **Import selected**. Only direct,
downloadable model files (`.stl/.3mf/.obj/.step/.stp`) are listed; the chosen file is
downloaded to your OrcaSlicer *download path* and loaded like any other import.

## Architecture

All AI code lives beside the rest of the slicer sources and links into the normal GUI
library — there are no new dependencies (HTTP via `Slic3r::Http`, JSON via
`nlohmann::json`).

| Component | Files | Role |
|---|---|---|
| **Providers** | `src/slic3r/Utils/AIProvider.{hpp,cpp}` | Abstract `AIProvider` + factory. Concrete OpenAI / Anthropic / OpenAI-compatible backends behind `chat()`, `test_connection()`, `get_models()`. Mirrors the `PrintHost::get_print_host` pattern. |
| **Machine context** | `src/slic3r/Utils/AIPrinterContext.{hpp,cpp}` | Assembles a structured snapshot: active slicer settings (always), live Moonraker/OctoPrint telemetry (when a Physical Printer is configured), and a camera JPEG (base64). Degrades gracefully offline. |
| **Text-to-shape** | `src/slic3r/Utils/AIShapeGen.{hpp,cpp}` | Pure libslic3r. Validates a JSON *shape spec* (primitives + CSG + transforms) or raw STL and bakes it into a `Model`; plus a bed-fit check. |
| **Settings UI** | `src/slic3r/GUI/AISettingsDialog.{hpp,cpp}` | Registers provider / key / gateway / model into the `ai_slicer` AppConfig section. |
| **Workspace UI** | `src/slic3r/GUI/AISlicerDialog.{hpp,cpp}` | *Generate shape* and *Search & import* tabs; wires context → provider → geometry → plate. |
| **Menu** | `src/slic3r/GUI/MainFrame.cpp` | The top-level **AI** menu. |

### How machine context reaches the model
On every generation `AIPrinterContext::gather()` builds a compact JSON blob that becomes
part of the system prompt, so the LLM knows the bed shape/size, nozzle, material and
temperatures, live sensor state, and (for vision models) sees a camera frame passed as a
multimodal image. `AIShapeGen::ai_model_fits_bed()` then checks the result against the
active build volume. This is what keeps generated geometry *printable on this machine*
rather than generically sized.

### Configuration keys (`ai_slicer` section of the OrcaSlicer config)
| Key | Meaning |
|---|---|
| `provider` | `openai` \| `anthropic` \| `compatible` (empty = disabled) |
| `api_key` | provider API key |
| `gateway_url` | base URL for the compatible provider |
| `model` | model id (e.g. `gpt-4o-mini`, `claude-3-5-sonnet-latest`) |

### Adding a provider
Implement a subclass of `AIProvider` (see `OpenAIProvider` / `AnthropicProvider` in
`AIProvider.cpp`), then add a branch to `AIProvider::create()` keyed on the `provider`
string. Nothing else needs to change — the dialogs and context flow are provider-agnostic.

## Security & privacy
- The API key is stored **in cleartext** in the OrcaSlicer config file, like other host
  credentials in the app. Treat that file accordingly; prefer a scoped/revocable key.
- Prompts include your machine context and (if a camera is configured) a snapshot — this
  is sent to the provider you configured. Nothing is sent until you invoke an AI action.
- *Search & import* downloads a file from a URL the model returned; only recognized model
  extensions are offered, and the file goes through the normal importer.

## Upstreaming notes
- Self-contained and opt-in; no new third-party dependencies.
- Delivered as a stack of small, compile-verified PRs: providers → context+multimodal →
  settings dialog → shape core → generate dialog → AI menu → search & import → docs.
- Known follow-ups: run the LLM round-trips on a background `Job` (currently a
  `wxBusyCursor`), and add a secret-store option for the API key.
