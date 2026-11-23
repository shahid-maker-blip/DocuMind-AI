# DocuMind AI

**An AI-powered desktop application for document management, analysis, and organization.**

DocuMind AI combines a C++ desktop interface with a Python backend and locally running AI to help users work with their documents.

## Features

- **Document Management:** Upload, browse, search, categorize, and organize documents.
- **AI Summarization:** Generate short and detailed summaries.
- **Document Chat:** Ask questions about document content.
- **Information Extraction:** Identify people, organizations, dates, financial details, and action items.
- **Document Comparison:** Compare two documents using AI.
- **Smart Organization:** Detect duplicates, manage tags, and view document statistics.
- **Additional Tools:** OCR support, report exports, and light/dark themes.

## Technology Stack

| Technology | Purpose |
|---|---|
| C++17, Dear ImGui | Desktop application |
| SDL2 | Window and rendering |
| SQLite3 | Local database |
| Python, FastAPI | AI backend |
| Ollama, Qwen 2.5 3B | Local AI processing |
| Tesseract OCR | Text recognition |

## Installation

**Requirements:** Windows 10/11, MSYS2 UCRT64, CMake, Ninja, Python 3, and Ollama.

1. Clone the repository:

   ```bash
   git clone https://github.com/shahid-maker-blip/DocuMind-AI.git
   cd DocuMind-AI
   ```

2. Build the desktop application:

   ```bash
   cmake -S . -B build -G Ninja
   cmake --build build
   ```

3. Install the AI backend dependencies:

   ```bash
   cd ai_service
   python -m pip install -r requirements.txt
   ```

4. Install Ollama and download the AI model:

   ```bash
   ollama pull qwen2.5:3b
   ```

## Privacy

DocuMind AI is designed for local document management and AI processing. Documents and application records are stored locally. Database files, personal documents, and generated build files are excluded from the source repository.

## Author

**Shahid Raza**

[GitHub Profile](https://github.com/shahid-maker-blip) · [Project Repository](https://github.com/shahid-maker-blip/DocuMind-AI)

---

*DocuMind AI — Smarter document management with local artificial intelligence.*
