# DocuMind AI

**An AI-powered desktop application for intelligent document management, analysis, and organization.**

DocuMind AI helps users manage their documents, extract important information, generate AI-powered summaries, compare files, and interact with their documents using a locally running AI model.

Built with C++, Dear ImGui, SQLite, Python, FastAPI, and Ollama, DocuMind AI combines desktop document management with local artificial intelligence.

## Features

### Document Management
- Upload and manage documents.
- Browse, search, and organize your document library.
- Categorize documents and assign custom tags.
- View document contents.
- Manage document details and metadata.

### AI-Powered Tools
- Generate short and detailed document summaries.
- Ask questions about individual documents.
- Extract important information, including:
  - People and organizations
  - Dates and deadlines
  - Contact details
  - Financial information
  - Tasks and action items
  - Decisions and key points
- Compare two documents using AI.
- Save and revisit AI conversations.

### Smart Organization and Analytics
- Detect duplicate documents by comparing file contents.
- View document statistics and storage usage.
- Analyze document categories and file types.
- Filter documents using categories and tags.

### Additional Features
- Light and dark appearance modes.
- OCR support for scanned PDF documents.
- Export extracted information and comparison reports.
- Local SQLite database for document metadata and application records.

## Technology Stack

| Technology | Purpose |
|---|---|
| C++17 | Desktop application |
| Dear ImGui | Graphical user interface |
| SDL2 | Window and rendering support |
| SQLite3 | Local database |
| Python | AI backend |
| FastAPI | Backend API |
| Ollama | Local AI inference |
| Qwen 2.5 3B | AI language model |
| Tesseract OCR | Text recognition |

## Project Structure

```text
DocuMind/
├── desktop/
│   ├── src/
│   │   └── main.cpp
│   └── imgui/
│
├── ai_service/
│   ├── main.py
│   └── requirements.txt
│
├── database/
├── documents/
├── exports/
├── build/
│
├── .gitignore
└── README.md
```

## Requirements

- Windows 10 or Windows 11
- MSYS2 UCRT64 environment
- GCC / G++ compiler
- CMake
- Ninja build system
- SDL2
- SQLite3
- Python 3
- Ollama

## Installation and Setup

### 1. Clone the Repository

```bash
git clone https://github.com/shahid-maker-blip/DocuMind-AI.git
cd DocuMind-AI
```

### 2. Build the Desktop Application

Configure the project:

```bash
cmake -S . -B build -G Ninja
```

Build:

```bash
cmake --build build
```

Run:

```bash
./build/DocuMind.exe
```

### 3. Set Up the AI Backend

Open a terminal and navigate to the backend:

```powershell
cd ai_service
```

Create a Python virtual environment:

```powershell
python -m venv .venv
```

Activate it:

```powershell
.\.venv\Scripts\Activate.ps1
```

Install dependencies:

```powershell
python -m pip install -r requirements.txt
```

Start the API server:

```powershell
python -m uvicorn main:app --host 127.0.0.1 --port 8000
```

### 4. Configure Local AI

Install and start Ollama, then download the model:

```bash
ollama pull qwen2.5:3b
```

The application uses the local Ollama service for AI-powered document operations.

## API Endpoints

The Python backend provides API functionality for document processing and AI operations.

API documentation:

```text
http://127.0.0.1:8000/docs
```

Health check:

```text
http://127.0.0.1:8000/health
```

## Privacy and Data

DocuMind AI is designed around local document management and local AI processing.

- Documents are managed in the local application.
- SQLite stores application records.
- Ollama provides local AI inference.
- Database files, personal documents, exports, and virtual environments are excluded from the Git repository through `.gitignore`.

**Note:** Local processing does not automatically guarantee complete security. Users should protect their computer, database, and document storage.

## Development Status

DocuMind AI is an actively developed project. New features and improvements may be added over time.

## Author

**Shahid**

GitHub: [@shahid-maker-blip](https://github.com/shahid-maker-blip)

## Repository

[https://github.com/shahid-maker-blip/DocuMind-AI](https://github.com/shahid-maker-blip/DocuMind-AI)

---

*DocuMind AI — Making document management smarter with local artificial intelligence.*
