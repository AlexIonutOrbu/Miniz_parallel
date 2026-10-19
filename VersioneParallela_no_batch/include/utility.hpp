#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "config.hpp"
#include "miniz.h"

// Header salvato nei primi byte del file compresso a blocchi
struct BigFileHeader {
    uint32_t magic = 0x42494746;  // "BIGF" in ASCII per riconoscere il formato a blocchi
    uint32_t num_chunks = 0;      // Numero totale di blocchi
    uint64_t original_size = 0;   // Dimensione originale del file in byte
};

// Risultato dell'elaborazione di un singolo blocco
struct ChunkResult {
    std::size_t chunk_id;
    std::vector<char> data;
    bool ok;
};
namespace fs = std::filesystem;
inline bool is_big_compressed_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return false;
    uint32_t magic = 0;
    ifs.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return (magic == 0x42494746);  // "BIGF"
}
inline bool isDirectory(const char* path) {
    std::error_code ec;
    const fs::path p(path);
    // check symlinks
    const auto st = fs::symlink_status(p, ec);
    if (ec) return false;
    // true only for real directories (a symlink-to-dir returns false)
    if (fs::is_directory(st)) return true;
    return false;
}

inline bool should_process(const std::filesystem::path& p) {
    const std::string ext = p.extension().string();  // file extension
    if (cfg.mode == COMP) {
        if (ext == cfg.out_ext || ext == ".tmp")
            return false;  // already compressed or temporary
        return true;
    } else {                          // DECOMP
        return (ext == cfg.out_ext);  // decompress only files with proper suffix
    }
}

namespace {
constexpr std::size_t CHUNK = 256 * 1024;

inline bool compress_stream(std::istream& in, std::ostream& out, int level) {
    z_stream s{};  // struttura dati per mantenere lo stato e i contatori della compressione

    if (deflateInit(&s, level) != Z_OK) return false;  // allocazione e inizializzazione della memoria interna di Miniz

    std::vector<unsigned char> inbuf(CHUNK), outbuf(CHUNK);  // vettori di buffer temporanei per i dati grezzi (input) e compressi (output)
    bool ok = true;
    int zret = Z_OK;

    while (in) {
        // Leggiamo un chunk di dati (fino a 256KB) dallo stream di input
        in.read(reinterpret_cast<char*>(inbuf.data()), static_cast<std::streamsize>(inbuf.size()));
        s.avail_in = static_cast<uInt>(in.gcount());  // quanti byte effettivi sono stati letti
        s.next_in = inbuf.data();

        // Se il file è finito ordiniamo a Miniz di completare l'archivio (Z_FINISH), altrimenti proseguiamo normalmente (Z_NO_FLUSH)
        const int flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;

        do {
            // Configura il buffer di output prima di ogni chiamata a deflate
            s.avail_out = static_cast<uInt>(outbuf.size());
            s.next_out = outbuf.data();

            // Esegue l'algoritmo di compressione DEFLATE
            zret = deflate(&s, flush);
            if (zret == Z_STREAM_ERROR) {
                ok = false;
                break;
            }

            // Calcola quanti byte compressi sono stati generati in questo passaggio e li scrive sul file di output
            const std::size_t have = outbuf.size() - s.avail_out;
            if (have) out.write(reinterpret_cast<const char*>(outbuf.data()), static_cast<std::streamsize>(have));
            if (!out) {
                // Gestione errore in caso di fallimento della scrittura su disco (es. disco pieno)
                ok = false;
                break;
            }
            // Il ciclo continua se avail_out è 0, segno che il buffer è stato riempito del tutto e Miniz ha ancora dati in attesa
        } while (s.avail_out == 0);

        // Interrompe il ciclo esterno se si è verificato un errore
        if (!ok) break;
        // Se abbiamo raggiunto la fine del file di input e Miniz ha completato con successo l'output, usciamo
        if (flush == Z_FINISH && zret == Z_STREAM_END) break;
    }
    deflateEnd(&s);  // Dealloca la memoria interna di Miniz per evitare memory leak
    return ok;
}

inline bool decompress_stream(std::istream& in, std::ostream& out) {
    constexpr std::size_t CHUNK = 256 * 1024;
    std::vector<unsigned char> inbuf(CHUNK), outbuf(CHUNK);  // Buffer per dati compressi (input) e dati espansi (output)

    z_stream s{};  // Struttura dati per mantenere lo stato e i contatori della decompressione
    if (inflateInit(&s) != Z_OK)
        return false;  // Inizializzazione della memoria interna per l'algoritmo di decompressione

    int zret = Z_OK;
    bool ok = true;

    for (;;) {
        // Lettura pigra (lazy): carichiamo nuovi dati dal file solo quando il buffer di input si svuota completamente
        if (s.avail_in == 0) {
            in.read(reinterpret_cast<char*>(inbuf.data()), static_cast<std::streamsize>(inbuf.size()));
            s.avail_in = static_cast<uInt>(in.gcount());  // Numero di byte compressi effettivamente letti dal disco
            s.next_in = inbuf.data();                     // Puntatore all'inizio dei dati da decomprimere
        }

        // Configurazione del buffer di output per accogliere i dati estratti
        s.avail_out = static_cast<uInt>(outbuf.size());
        s.next_out = outbuf.data();

        // Esecuzione dell'algoritmo di decompressione (Z_NO_FLUSH poiché la fine è determinata dai metadati del flusso)
        zret = inflate(&s, Z_NO_FLUSH);

        // Caso Successo: Miniz ha rilevato la fine naturale del flusso compresso
        if (zret == Z_STREAM_END) {
            const std::size_t have = outbuf.size() - s.avail_out;  // Calcola gli ultimi byte estratti
            if (have) out.write(reinterpret_cast<const char*>(outbuf.data()), static_cast<std::streamsize>(have));
            ok = static_cast<bool>(out);
            inflateEnd(&s);  // Deallocazione della memoria per evitare memory leak
            return ok;
        }

        // Caso Errore: Rilevamento di corruzione dati, errori di memoria o di consistenza dello stream
        if (zret == Z_NEED_DICT || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR || zret == Z_STREAM_ERROR) {
            inflateEnd(&s);
            return false;
        }

        // Scrittura intermedia: se sono stati generati dei byte estratti, li salviamo sul disco
        const std::size_t have = outbuf.size() - s.avail_out;
        if (have) {
            out.write(reinterpret_cast<const char*>(outbuf.data()), static_cast<std::streamsize>(have));
            if (!out) {
                inflateEnd(&s);
                return false;
            }  // Interruzione in caso di fallimento della scrittura
        }

        // Controllo anti-stallo: il file di input è terminato prematuramente senza raggiungere il reale Z_STREAM_END
        if (s.avail_in == 0 && in.eof() && zret == Z_BUF_ERROR) {
            inflateEnd(&s);
            return false;
        }
    }
}
inline fs::path make_output_path(const fs::path& in) {
    if (cfg.mode == COMP) {
        return fs::path(in.string() + cfg.out_ext);
    } else {
        const std::string s = in.string();
        const std::string& ext = cfg.out_ext;
        if (s.size() >= ext.size() && s.rfind(ext) == s.size() - ext.size()) {
            return fs::path{s.substr(0, s.size() - ext.size())};
        }
        return fs::path{s + ".raw"};
    }
}
}  // namespace

// apply compression or decompression on a regular file
inline bool doWork(const char* path_cstr) {
    const fs::path in(path_cstr);
    /*
        Controlla che il file esista, potrebbe essere stato eliminato durante l'esecuzione del programma
    */
    if (!fs::exists(in)) {
        std::fprintf(stderr, "File disappear: %s\n", path_cstr);
        return false;
    }
    /*
        Se il file esiste controlla che sia effettivamente un file (e non una directory o un link simbolico)
    */
    if (!fs::is_regular_file(in)) {
        if (cfg.verbose) std::fprintf(stderr, "Skip, not a regular file: %s\n", path_cstr);
        return true;
    }

    const fs::path out_final = make_output_path(in);                 // crea il path di destinazione
    const fs::path out_tmp = fs::path(out_final.string() + ".tmp");  // crea il path finale di destinazione temporaneo

    if (cfg.verbose) {
        if (cfg.mode == COMP)
            std::fprintf(stderr, "[C] %s -> %s\n", in.string().c_str(), out_final.string().c_str());
        else
            std::fprintf(stderr, "[D] %s -> %s\n", in.string().c_str(), out_final.string().c_str());
    }

    std::error_code ec_eq;
    // controlla che siano diversi
    if (fs::equivalent(in, out_final, ec_eq) && !ec_eq) {
        std::fprintf(stderr, "Error: Output coincide with input: %s\n", out_final.string().c_str());
        return false;
    }
    // crea lo stream di input
    std::ifstream ifs(in, std::ios::binary);
    if (!ifs) {
        std::fprintf(stderr, "Error: cannot open in input: %s\n", in.string().c_str());
        return false;
    }
    // crea lo stream di output temporaneo
    std::ofstream ofs(out_tmp, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        std::fprintf(stderr, "Error: cannot open in output: %s\n", out_tmp.string().c_str());
        return false;
    }
    // chiama compressione/decompressione
    bool ok = (cfg.mode == COMP) ? compress_stream(ifs, ofs, cfg.level) : decompress_stream(ifs, ofs);
    ofs.close();

    if (!ok) {
        std::error_code ecc;
        fs::remove(out_tmp, ecc);  // remove temporary file
        std::fprintf(stderr, "Error: %s on %s\n",
            (cfg.mode == COMP ? "compression" : "decompression"),
            in.string().c_str());
        return false;
    }

    // atomic raname, if it is not supported by the FS, fallback to copy+remove
    std::error_code ec;
    fs::rename(out_tmp, out_final, ec);
    if (ec) {  // rename failed
        std::error_code ecw, ecr;
        fs::copy_file(out_tmp, out_final, fs::copy_options::overwrite_existing, ecw);
        fs::remove(out_tmp, ecr);  // remove temporary file
        if (ecw) {
            std::fprintf(stderr, "Error: impossible to write output: %s\n", out_final.string().c_str());
            return false;
        }
    }
    // do we have to remove the input file?
    if (cfg.remove_input) {
        std::error_code ecr;
        fs::remove(in, ecr);
        if (ecr && cfg.verbose)
            std::fprintf(stderr, "Warning: file %s has not been removed (%s)\n",
                in.string().c_str(), ecr.message().c_str());
    }
    return true;
}
inline bool doWorkBigFile(const char* path_cstr, threadPool& pool) {
    const fs::path in(path_cstr);

    if (!fs::exists(in)) {
        std::fprintf(stderr, "File scomparso: %s\n", path_cstr);
        return false;
    }
    if (!fs::is_regular_file(in)) {
        if (cfg.verbose) std::fprintf(stderr, "Skip, non è un file regolare: %s\n", path_cstr);
        return true;
    }

    const fs::path out_final = make_output_path(in);
    const fs::path out_tmp = fs::path(out_final.string() + ".tmp");

    if (cfg.verbose) {
        std::fprintf(stderr, "[%s BIG] %s -> %s\n",
            (cfg.mode == COMP ? "C" : "D"), in.string().c_str(), out_final.string().c_str());
    }

    std::error_code ec_eq;
    if (fs::equivalent(in, out_final, ec_eq) && !ec_eq) {
        std::fprintf(stderr, "Errore: Output coincide con l'input: %s\n", out_final.string().c_str());
        return false;
    }

    // =========================================================================
    // RAMO 1: COMPRESSIONE DI UN FILE GRANDE
    // =========================================================================
    if (cfg.mode == COMP) {
        std::ifstream ifs(in, std::ios::binary);
        if (!ifs) return false;

        const std::size_t fsize = fs::file_size(in);
        const std::size_t chunk_size = cfg.block_size;  // parametro -b (es. 1 MB)
        const uint32_t num_chunks = static_cast<uint32_t>((fsize + chunk_size - 1) / chunk_size);

        std::vector<std::future<ChunkResult>> F;
        F.reserve(num_chunks);

        // 1. Lettura a blocchi e invio task al Thread Pool
        for (uint32_t i = 0; i < num_chunks; ++i) {
            std::size_t bytes_to_read = std::min(chunk_size, fsize - i * chunk_size);
            std::vector<char> buffer(bytes_to_read);
            ifs.read(buffer.data(), static_cast<std::streamsize>(bytes_to_read));

            F.emplace_back(pool.submit([chunk_id = i, buf = std::move(buffer)]() -> ChunkResult {
                std::string in_str(buf.data(), buf.size());
                std::istringstream iss(in_str, std::ios::binary);
                std::ostringstream oss(std::ios::binary);

                bool ok = compress_stream(iss, oss, cfg.level);
                std::string out_str = oss.str();

                return ChunkResult{
                    chunk_id,
                    std::vector<char>(out_str.begin(), out_str.end()),
                    ok};
            }));
        }
        ifs.close();

        // 2. Rifiuto ed attesa dei risultati + Scrittura del File Compresso
        std::ofstream ofs(out_tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;

        // Scriviamo l'Header
        BigFileHeader header{0x42494746, num_chunks, static_cast<uint64_t>(fsize)};
        ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));

        // Raccogliamo tutti i blocchi compressi e calcoliamo le loro dimensioni
        std::vector<ChunkResult> results(num_chunks);
        std::vector<uint32_t> comp_sizes(num_chunks);
        bool all_ok = true;

        for (uint32_t i = 0; i < num_chunks; ++i) {
            results[i] = F[i].get();
            all_ok &= results[i].ok;
            comp_sizes[i] = static_cast<uint32_t>(results[i].data.size());
        }

        if (!all_ok) {
            ofs.close();
            fs::remove(out_tmp);
            return false;
        }

        // Scriviamo la tabella delle dimensioni dei blocchi compressi
        ofs.write(reinterpret_cast<const char*>(comp_sizes.data()), num_chunks * sizeof(uint32_t));

        // Scriviamo il contenuto dei blocchi compressi in ordine
        for (uint32_t i = 0; i < num_chunks; ++i) {
            ofs.write(results[i].data.data(), results[i].data.size());
        }
        ofs.close();
    }
    // =========================================================================
    // RAMO 2: DECOMPRESSIONE DI UN FILE GRANDE
    // =========================================================================
    else {
        std::ifstream ifs(in, std::ios::binary);
        if (!ifs) return false;

        // Leggiamo l'Header
        BigFileHeader header;
        ifs.read(reinterpret_cast<char*>(&header), sizeof(header));

        if (header.magic != 0x42494746) {
            std::fprintf(stderr, "Errore: Formato file grande non valido o corrotto: %s\n", path_cstr);
            return false;
        }

        // Leggiamo la tabella delle dimensioni dei blocchi compressi
        std::vector<uint32_t> comp_sizes(header.num_chunks);
        ifs.read(reinterpret_cast<char*>(comp_sizes.data()), header.num_chunks * sizeof(uint32_t));

        std::vector<std::future<ChunkResult>> F;
        F.reserve(header.num_chunks);

        // Inviamo i task di decompressione al Thread Pool
        for (uint32_t i = 0; i < header.num_chunks; ++i) {
            std::vector<char> comp_buf(comp_sizes[i]);
            ifs.read(comp_buf.data(), comp_sizes[i]);

            F.emplace_back(pool.submit([chunk_id = i, buf = std::move(comp_buf)]() -> ChunkResult {
                std::string in_str(buf.data(), buf.size());
                std::istringstream iss(in_str, std::ios::binary);
                std::ostringstream oss(std::ios::binary);

                bool ok = decompress_stream(iss, oss);
                std::string out_str = oss.str();

                return ChunkResult{
                    chunk_id,
                    std::vector<char>(out_str.begin(), out_str.end()),
                    ok};
            }));
        }
        ifs.close();

        // Scriviamo il file decompresso finale
        std::ofstream ofs(out_tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;

        bool all_ok = true;
        for (uint32_t i = 0; i < header.num_chunks; ++i) {
            ChunkResult res = F[i].get();
            all_ok &= res.ok;
            ofs.write(res.data.data(), res.data.size());
        }
        ofs.close();

        if (!all_ok) {
            fs::remove(out_tmp);
            return false;
        }
    }

    // Rinominamento atomico e rimozione file sorgente (uguale a doWork)
    std::error_code ec;
    fs::rename(out_tmp, out_final, ec);
    if (ec) {
        std::error_code ecw, ecr;
        fs::copy_file(out_tmp, out_final, fs::copy_options::overwrite_existing, ecw);
        fs::remove(out_tmp, ecr);
        if (ecw) return false;
    }

    if (cfg.remove_input) {
        std::error_code ecr;
        fs::remove(in, ecr);
    }

    return true;
}
inline bool walkDirParallel(const char* dir_cstr, threadPool& pool) {
    const fs::path root(dir_cstr);
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::fprintf(stderr, "Non è una directory: %s\n", dir_cstr);
        return false;
    }
    bool all_ok = true;
    std::vector<std::future<bool>> F;
    auto handle_file = [&](const fs::path& p, std::size_t fsize) {
        bool is_big = (cfg.mode == COMP) ? (fsize >= cfg.big_file_size) : is_big_compressed_file(p);

        if (is_big) {
            // Chiamata DIRETTA dal thread principale:
            // doWorkBigFile userà il threadPool per parallelizzare i suoi chunk!
            bool ok = doWorkBigFile(p.string().c_str(), pool);
            if (!ok) all_ok = false;
        } else {
            // Per i file piccoli inviamo il task al pool come prima
            F.emplace_back(pool.submit([](std::string path_str) {
                return doWork(path_str.c_str());
            },
                p.string()));
        }
    };
    if (cfg.recursive) {
        fs::directory_options opts = fs::directory_options::skip_permission_denied;
        for (auto it = fs::recursive_directory_iterator(root, opts),
                  end = fs::recursive_directory_iterator();
            it != end; ++it) {
            std::error_code ec;
            if (it->is_symlink(ec) || it->is_directory(ec)) continue;
            if (it->is_regular_file(ec)) {
                if (!should_process(it->path())) {
                    if (cfg.verbose)
                        std::fprintf(stderr, "Skip: %s\n", it->path().string().c_str());
                    continue;
                }
                std::size_t fsize = it->file_size(ec);
                handle_file(it->path(), fsize);
            }
        }
    } else {
        for (auto& de : fs::directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            std::error_code ec;
            if (de.is_regular_file(ec)) {
                if (!should_process(de.path())) {
                    if (cfg.verbose)
                        std::fprintf(stderr, "Skip: %s\n", de.path().string().c_str());
                    continue;
                }
                std::size_t fsize = de.file_size(ec);
                handle_file(de.path(), fsize);
            }
        }
    }
    for (size_t i = 0; i < F.size(); ++i) {
        all_ok &= F[i].get();
    }
    return all_ok;
}