#include <gtkmm.h>
#include <gio/gio.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <gtkmm/treeview.h>
#include <gtkmm/treestore.h>
#include <gtkmm/liststore.h>
#include <array>
#include <algorithm>
#include <cctype>
#include <gtkmm/image.h>
#include <gdkmm/pixbuf.h>
#include <gst/gst.h>
#include <thread>
#include <atomic>
#include <map>
#include <iostream>
#include <regex>
#include <iomanip>
#include <utility>
#include <vector>
#include <sqlite3.h>
#include <alsa/asoundlib.h>

#include <unistd.h> // For readlink
#include <limits.h> // For PATH_MAX
// Richiede C++17
namespace fs = std::filesystem;

// Flag globale per il controllo del debug sulla shell
std::atomic<bool> g_debug_enabled{true};

// Helper per assicurarsi che una stringa sia UTF-8 valida per GTK
std::string sanitize_utf8(const std::string& s) {
    if (s.empty()) return "";
    if (g_utf8_validate(s.c_str(), s.size(), NULL)) return s;
    
    gsize bytes_read, bytes_written;
    char* converted = g_convert_with_fallback(s.c_str(), s.size(), "UTF-8", "ISO-8859-1", "?", &bytes_read, &bytes_written, NULL);
    if (converted) {
        std::string result(converted, bytes_written);
        g_free(converted);
        return result;
    }
    return "[Errore Codifica]";
}

#define LOG(severity, message) \
    do { \
        if (g_debug_enabled || #severity[0] != 'D') { \
            std::cerr << "[" << #severity << "] " << __FILE__ << ":" << __LINE__ << " " << __FUNCTION__ << "() " << message << std::endl; \
        } \
    } while (0)

// Nome del file di configurazione
const std::string CONFIG_FILENAME = ".music_config";
const std::string DB_FILENAME = "music_library.db";

const std::string APP_VERSION = "0.1";

struct SambaConfig {
    std::string path;
    std::string username;
    std::string password;
};

// Funzione per salvare la configurazione su file
void saveConfig(const SambaConfig& config) {
    LOG(INFO, "Saving config");
    std::ofstream file(CONFIG_FILENAME);
    if (file.is_open()) {
        file << config.path << "\n";
        file << config.username << "\n";
        file << config.password << "\n";
        file.close();
    }
    LOG(INFO, "Config saved");
}

// Funzione per caricare la configurazione
bool loadConfig(SambaConfig& config) {
    LOG(INFO, "Loading config");
    if (!fs::exists(CONFIG_FILENAME)) {
        return false; // File non esiste, è il primo avvio
    }

    std::ifstream file(CONFIG_FILENAME);
    if (file.is_open()) {
        std::getline(file, config.path);
        std::getline(file, config.username);
        std::getline(file, config.password);
        file.close();
        LOG(INFO, "Config loaded");
        return true;
    }
    LOG(ERROR, "Failed to load config");
    return false;
}

std::string escape_for_double_quotes(const std::string& s) {
    std::string result;
    for (char c : s) {
        // Escape characters that have special meaning inside double quotes in shell
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            result += '\\';
        }
        result += c;
    }
    return result;
}


std::pair<bool, std::string> exec_smb_command(const std::string& path, const SambaConfig& config, const std::string& smb_cmd) {
    std::string full_path = path;

    // Normalizza il percorso: rimuove smb:, converte backslashes e assicura // iniziale
    if (full_path.length() >= 4 && full_path.substr(0, 4) == "smb:") full_path.erase(0, 4);
    std::replace(full_path.begin(), full_path.end(), '\\', '/');
    size_t start_pos = full_path.find_first_not_of('/');
    if (start_pos == std::string::npos) start_pos = 0;
    full_path = "//" + full_path.substr(start_pos);

    std::string service = full_path;
    std::string directory = "";

    // Tenta di separare //server/share da /percorso/sottocartella
    // Esempio: //host/share/dir -> service="//host/share", directory="dir"
    if (full_path.length() > 2 && full_path.substr(0, 2) == "//") {
        size_t first_slash = full_path.find('/', 2); // Trova slash dopo host
        if (first_slash != std::string::npos) {
            size_t second_slash = full_path.find('/', first_slash + 1); // Trova slash dopo share
            if (second_slash != std::string::npos) {
                service = full_path.substr(0, second_slash);
                directory = full_path.substr(second_slash + 1);
            }
        }
    }

    // Escape double quotes in smb_cmd for shell usage inside -c "..."
    std::string escaped_cmd = smb_cmd;
    size_t pos = 0;
    while ((pos = escaped_cmd.find("\"", pos)) != std::string::npos) {
        escaped_cmd.replace(pos, 1, "\\\"");
        pos += 2;
    }

    std::string command;
    std::string escaped_directory = escape_for_double_quotes(directory);
    std::string escaped_username = escape_for_double_quotes(config.username);
    std::string escaped_password = escape_for_double_quotes(config.password);

    if (!directory.empty()) {
        command = "smbclient \"" + service + "\" -D \"" + escaped_directory + "\" -U \"" + 
                  escaped_username + "%" + escaped_password + "\" -c \"" + escaped_cmd + "\" 2>&1";
    } else {
        command = "smbclient \"" + service + "\" -U \"" + 
                  escaped_username + "%" + escaped_password + "\" -c \"" + escaped_cmd + "\" 2>&1";
    }
    
    // LOG(DEBUG, "Executing command: " << command);

    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    
    if (!pipe) return std::make_pair(false, "Critical error: could not execute smbclient.");
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {

        result += buffer.data();
    }
    return std::make_pair(true, result);
}

// Funzione per eseguire smbclient e catturare l'output (wrapper per ls)
std::string exec_smb(const std::string& path, const SambaConfig& config)
{
    return exec_smb_command(path, config, "ls").second;
}

bool download_smb_file(const std::string& full_path, const std::string& local_path, const SambaConfig& config) {
    size_t last_slash = full_path.find_last_of('/');
    if (last_slash == std::string::npos) {
        LOG(ERROR, "Invalid full_path for download (no slash): " << full_path);
        return false;
    }
    
    std::string folder_path = full_path.substr(0, last_slash);
    std::string filename = full_path.substr(last_slash + 1);
    
    // Comando 'get' per smbclient
    std::string smb_cmd = "get \"" + filename + "\" \"" + local_path + "\"";
    
    // Usiamo la funzione centralizzata per eseguire il comando
    std::pair<bool, std::string> result = exec_smb_command(folder_path, config, smb_cmd);

    // Controlla se l'esecuzione di popen è fallita
    if (!result.first) {
        LOG(ERROR, "Failed to execute popen for smbclient download: " << result.second);
        return false;
    }

    // Controlla se il file locale esiste e non è vuoto
    bool success = fs::exists(local_path);
    if (success) {
        try {
            if (fs::file_size(local_path) == 0) {
                success = false;
            }
        } catch (const fs::filesystem_error& e) {
            LOG(ERROR, "Filesystem error checking file size for " << local_path << ": " << e.what());
            success = false;
        }
    }

    // Se il file non è stato creato, logga l'output di smbclient per debug
    if (!success) {
        LOG(ERROR, "Download failed for SMB path: " << full_path);
        LOG(ERROR, "smbclient output: " << result.second);
        // Prova a rimuovere un file potenzialmente vuoto o corrotto
        if(fs::exists(local_path)) fs::remove(local_path);
    }
    
    return success;
}

bool download_smb_to_memory(const std::string& full_path, std::vector<char>& data, const SambaConfig& config) {
    std::string service = full_path;
    std::string directory = "";

    if (full_path.length() > 2 && full_path.substr(0, 2) == "//") {
        size_t first_slash = full_path.find('/', 2);
        if (first_slash != std::string::npos) {
            size_t second_slash = full_path.find('/', first_slash + 1);
            if (second_slash != std::string::npos) {
                service = full_path.substr(0, second_slash);
                directory = full_path.substr(second_slash + 1);
            }
        }
    }

    std::string filename = full_path.substr(full_path.find_last_of('/') + 1);
    
    std::string escaped_directory = escape_for_double_quotes(directory);
    std::string escaped_username = escape_for_double_quotes(config.username);
    std::string escaped_password = escape_for_double_quotes(config.password);
    std::string escaped_filename = escape_for_double_quotes(filename);

    // Usa '-' come output file per scrivere su stdout
    std::string command = "smbclient \"" + service + "\" -D \"" + escaped_directory + "\" -U \"" + 
                  escaped_username + "%" + escaped_password + "\" -c \"get \\\"" + escaped_filename + "\\\" -\"";

    // Non reindirizziamo stderr su stdout per non corrompere i dati binari con i log
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) return false;

    std::array<char, 8192> buffer;
    while (true) {
        size_t bytes_read = fread(buffer.data(), 1, buffer.size(), pipe.get());
        if (bytes_read > 0) {
            data.insert(data.end(), buffer.data(), buffer.data() + bytes_read);
        } else {
            break;
        }
    }
    return !data.empty();
}

// Helper per URL encoding
std::string url_encode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
        std::string::value_type c = (*i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase << '%' << std::setw(2) << std::hex << (int)((unsigned char)c) << std::nouppercase;
        }
    }
    return escaped.str();
}

class ConfigDialog : public Gtk::Dialog {
public:
    ConfigDialog(Gtk::Window& parent, SambaConfig& config) 
        : Gtk::Dialog("Configurazione Samba", parent, true), m_config(config) {
        
        set_default_size(400, 200);
        auto content = get_content_area();
        
        m_grid.set_row_spacing(10);
        m_grid.set_column_spacing(10);
        m_grid.set_border_width(10);

        m_lblPath.set_text("Percorso Samba:");
        m_lblUser.set_text("Utente:");
        m_lblPass.set_text("Password:");

        m_entryPath.set_text(config.path);
        m_entryUser.set_text(config.username);
        m_entryPass.set_text(config.password);
        m_entryPass.set_visibility(false); // Nasconde la password

        m_grid.attach(m_lblPath, 0, 0, 1, 1);
        m_grid.attach(m_entryPath, 1, 0, 1, 1);
        m_grid.attach(m_lblUser, 0, 1, 1, 1);
        m_grid.attach(m_entryUser, 1, 1, 1, 1);
        m_grid.attach(m_lblPass, 0, 2, 1, 1);
        m_grid.attach(m_entryPass, 1, 2, 1, 1);

        content->add(m_grid);
        
        add_button("Annulla", Gtk::RESPONSE_CANCEL);
        add_button("Salva e Connetti", Gtk::RESPONSE_OK);
        
        show_all_children();
    }

    void on_response(int response_id) override {
        if (response_id == Gtk::RESPONSE_OK) {
            m_config.path = m_entryPath.get_text();
            m_config.username = m_entryUser.get_text();
            m_config.password = m_entryPass.get_text();
        }
    }

protected:
    SambaConfig& m_config;
    Gtk::Grid m_grid;
    Gtk::Label m_lblPath, m_lblUser, m_lblPass;
    Gtk::Entry m_entryPath, m_entryUser, m_entryPass;
};

class ModelColumns : public Gtk::TreeModel::ColumnRecord
{
public:
    ModelColumns() { 
        add(m_col_name); add(m_col_path); add(m_col_icon); 
        // Colonne aggiuntive per la vista DB dettagliata
        add(m_col_artist); add(m_col_title); add(m_col_album);
        add(m_col_year); add(m_col_genre);
    }
    Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    Gtk::TreeModelColumn<Glib::ustring> m_col_path;
    Gtk::TreeModelColumn<Glib::ustring> m_col_icon;

    Gtk::TreeModelColumn<Glib::ustring> m_col_artist;
    Gtk::TreeModelColumn<Glib::ustring> m_col_title;
    Gtk::TreeModelColumn<Glib::ustring> m_col_album;
    Gtk::TreeModelColumn<Glib::ustring> m_col_year;
    Gtk::TreeModelColumn<Glib::ustring> m_col_genre;
};

class DeviceColumns : public Gtk::TreeModel::ColumnRecord
{
public:
    DeviceColumns() { add(m_col_name); add(m_col_class); add(m_col_device); }
    Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    Gtk::TreeModelColumn<Glib::ustring> m_col_class;
    Gtk::TreeModelColumn<GstDevice*> m_col_device;
};

struct Metadata {
    std::string artist;
    std::string title;
    std::string album;
    std::string year;
    std::string genre;
    std::string artworkUrl;
    std::string coverPath;
    unsigned int bitrate = 0;
    std::string codec;
    unsigned int samplerate = 0;
    double size_mb = 0.0;
    std::vector<char> rawCoverData;
};

class PlayerWindow : public Gtk::Window {
    // Dichiarazioni dei membri riordinate per corrispondere all'ordine di inizializzazione nel costruttore
    // Risolve i warning -Wreorder
    std::atomic<int> m_selection_gen;
    std::atomic<int> m_folder_selection_gen;
    std::atomic<bool> m_stop_midi;
    bool m_automatic_cover_search_done; // Spostato all'interno della classe
    std::map<std::string, Metadata> m_meta_cache; // Spostato all'interno della classe

public:
    PlayerWindow() : 
        m_selection_gen(0), 
        m_folder_selection_gen(0), 
        m_stop_midi(false),
        m_automatic_cover_search_done(false), // Inizializzato qui
        m_pipeline(nullptr), 
        m_scale(Gtk::ORIENTATION_HORIZONTAL), 
        m_volume_scale(Gtk::ORIENTATION_HORIZONTAL) {
        set_title("Music Network Player");
        set_title("Music Network Player " + APP_VERSION);
        set_default_size(1024, 768);
        init_db();
        maximize();

        m_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
        add(m_box);

        // --- Top Bar ---
        m_top_bar.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_top_bar.set_spacing(10);
        m_top_bar.set_border_width(5);

        // Setup Slider nella Top Bar
        m_slider_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_slider_box.set_spacing(10);

        m_scale.set_range(0, 100);
        m_scale.set_draw_value(false);
        m_scale.set_hexpand(true);
        m_scale.signal_value_changed().connect(sigc::mem_fun(*this, &PlayerWindow::on_scale_value_changed));
        m_time_label.set_text("0:00 / 0:00");
        
        m_volume_scale.set_range(0, 100);
        m_volume_scale.set_draw_value(false);
        m_volume_scale.set_size_request(100, -1);
        m_volume_scale.set_value(100);
        m_volume_scale.signal_value_changed().connect(sigc::mem_fun(*this, &PlayerWindow::on_volume_changed));

        m_slider_box.pack_start(m_scale, Gtk::PACK_EXPAND_WIDGET);
        m_slider_box.pack_start(m_time_label, Gtk::PACK_SHRINK);
        m_slider_box.pack_start(m_volume_scale, Gtk::PACK_SHRINK);

        m_btn_play.set_label("Play");
        m_btn_play.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_play_clicked));

        m_btn_pause.set_label("Pausa");
        m_btn_pause.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_pause_clicked));

        m_btn_stop.set_label("Stop");
        m_btn_stop.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_stop_clicked));

        m_btnCast.set_label("Cerca Dispositivo");
        m_btnCast.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_cast_clicked));
        m_top_bar.pack_start(m_btnCast, Gtk::PACK_SHRINK);

        m_btnDB.set_label("DB");
        m_btnDB.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_db_view_toggled));
        m_top_bar.pack_start(m_btnDB, Gtk::PACK_SHRINK);

        m_btnScanDB.set_label("Scansiona Libreria");
        m_btnScanDB.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_scan_library_clicked));
        m_top_bar.pack_start(m_btnScanDB, Gtk::PACK_SHRINK);

        m_btnClearDB.set_label("Pulisci DB");
        m_btnClearDB.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_clear_db_clicked));
        m_top_bar.pack_start(m_btnClearDB, Gtk::PACK_SHRINK);

        m_btnRefresh.set_label("Aggiorna Lista");
        m_btnRefresh.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::refresh_list));
        m_top_bar.pack_start(m_btnRefresh, Gtk::PACK_SHRINK);

        m_btnDebugToggle.set_label("Debug On");
        m_btnDebugToggle.set_active(true);
        m_btnDebugToggle.signal_toggled().connect(sigc::mem_fun(*this, &PlayerWindow::on_debug_toggle));
        m_top_bar.pack_start(m_btnDebugToggle, Gtk::PACK_SHRINK);

        m_btnIpod.set_label("iPod");
        m_btnIpod.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_ipod_clicked));
        m_top_bar.pack_start(m_btnIpod, Gtk::PACK_SHRINK);

        m_btn_youtube.set_label("YouTube");
        m_btn_youtube.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_youtube_clicked));
        m_top_bar.pack_start(m_btn_youtube, Gtk::PACK_SHRINK);

        m_btnConfig.set_label("Riconfigura");
        m_btnConfig.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_config_clicked));
        m_top_bar.pack_start(m_btnConfig, Gtk::PACK_SHRINK);

        m_btnExit.set_label("Esci");
        m_btnExit.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_exit_clicked));
        m_top_bar.pack_start(m_btnExit, Gtk::PACK_SHRINK);

        m_box.pack_start(m_top_bar, Gtk::PACK_SHRINK);

        // --- Bottom Pane (3 columns) ---
        m_main_pane.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_box.pack_start(m_main_pane, Gtk::PACK_EXPAND_WIDGET);

        // --- Status Bar ---
        m_status_bar.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_status_bar.set_spacing(10);
        m_status_bar.set_border_width(2);
        m_status_bar.pack_start(m_scan_status_label, Gtk::PACK_SHRINK);
        m_status_bar.pack_start(m_scan_progress_bar, Gtk::PACK_EXPAND_WIDGET);
        m_box.pack_start(m_status_bar, Gtk::PACK_SHRINK);

        m_left_container.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_main_pane.pack1(m_left_container, true, false);
        m_left_container.set_size_request(200, -1); // Evita warning "negative content width" se il pannello è troppo piccolo

        m_folder_file_pane.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_folder_file_pane.set_homogeneous(true);

        // --- Colonna 1: Cartelle ---
        m_folder_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_folder_scrolled_window.add(m_folder_view);
        m_folder_file_pane.pack_start(m_folder_scrolled_window, Gtk::PACK_EXPAND_WIDGET);

        m_folder_model = Gtk::TreeStore::create(m_columns);
        m_folder_view.set_model(m_folder_model);
        
        auto pColFolder = Gtk::manage(new Gtk::TreeViewColumn("Cartelle"));
        auto pRenIconFolder = Gtk::manage(new Gtk::CellRendererPixbuf());
        auto pRenTextFolder = Gtk::manage(new Gtk::CellRendererText());
        pColFolder->pack_start(*pRenIconFolder, false);
        pColFolder->pack_start(*pRenTextFolder, true);
        pColFolder->add_attribute(pRenIconFolder->property_icon_name(), m_columns.m_col_icon);
        pColFolder->add_attribute(pRenTextFolder->property_text(), m_columns.m_col_name);
        pColFolder->set_sort_column(m_columns.m_col_name);
        m_folder_view.append_column(*pColFolder);
        m_folder_model->set_sort_column(m_columns.m_col_name, Gtk::SORT_ASCENDING);

        m_folder_view.get_selection()->signal_changed().connect(
            sigc::mem_fun(*this, &PlayerWindow::on_folder_selected));
        m_folder_view.set_headers_visible(true);
        m_folder_view.signal_row_expanded().connect(
            sigc::mem_fun(*this, &PlayerWindow::on_folder_expanded));
        m_folder_view.signal_button_press_event().connect(sigc::mem_fun(*this, &PlayerWindow::on_folder_button_press), false);

        // --- Colonna 2: File (per ora, output grezzo) ---
        m_middle_pane.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_folder_file_pane.pack_start(m_middle_pane, Gtk::PACK_EXPAND_WIDGET);

        m_files_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_files_scrolled_window.add(m_files_view);
        m_middle_pane.pack1(m_files_scrolled_window, true, false);
        m_files_model = Gtk::ListStore::create(m_columns);
        m_files_view.set_model(m_files_model);
        
        m_pColFile = Gtk::manage(new Gtk::TreeViewColumn("File"));
        auto pRenIconFile = Gtk::manage(new Gtk::CellRendererPixbuf());
        auto pRenTextFile = Gtk::manage(new Gtk::CellRendererText());
        m_pColFile->pack_start(*pRenIconFile, false);
        m_pColFile->pack_start(*pRenTextFile, true);
        m_pColFile->add_attribute(pRenIconFile->property_icon_name(), m_columns.m_col_icon);
        m_pColFile->add_attribute(pRenTextFile->property_text(), m_columns.m_col_name);
        m_pColFile->set_sort_column(m_columns.m_col_name);
        m_files_view.append_column(*m_pColFile);
        m_files_model->set_sort_column(m_columns.m_col_name, Gtk::SORT_ASCENDING);
        
        // Menu contestuale
        m_item_rename.set_label("Rinomina");
        m_item_delete.set_label("Elimina");
        m_context_menu.append(m_item_rename);
        m_context_menu.append(m_item_delete);
        m_context_menu.show_all();
        m_item_rename.signal_activate().connect(sigc::mem_fun(*this, &PlayerWindow::on_menu_rename));
        m_item_delete.signal_activate().connect(sigc::mem_fun(*this, &PlayerWindow::on_menu_delete));
        m_files_view.signal_button_press_event().connect(sigc::mem_fun(*this, &PlayerWindow::on_files_button_press), false);
        m_files_view.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &PlayerWindow::on_file_selected));
        m_files_view.signal_row_activated().connect(sigc::mem_fun(*this, &PlayerWindow::on_file_row_activated));

        // Setup Drag and Drop Sorgente (Samba Files)
        std::vector<Gtk::TargetEntry> listTargets;
        listTargets.push_back(Gtk::TargetEntry("text/plain"));
        m_files_view.enable_model_drag_source(listTargets, Gdk::BUTTON1_MASK, Gdk::ACTION_COPY);
        m_files_view.signal_drag_data_get().connect(sigc::mem_fun(*this, &PlayerWindow::on_files_drag_data_get));

        // --- Sezione iPod (Parte bassa Colonna 2) ---
        m_ipod_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_ipod_model = Gtk::ListStore::create(m_columns);
        m_ipod_view.set_model(m_ipod_model);
        auto pColIpod = Gtk::manage(new Gtk::TreeViewColumn("Contenuto iPod"));
        auto pRenIconIpod = Gtk::manage(new Gtk::CellRendererPixbuf());
        auto pRenTextIpod = Gtk::manage(new Gtk::CellRendererText());
        pColIpod->pack_start(*pRenIconIpod, false);
        pColIpod->pack_start(*pRenTextIpod, true);
        pColIpod->add_attribute(pRenIconIpod->property_icon_name(), m_columns.m_col_icon);
        pColIpod->add_attribute(pRenTextIpod->property_text(), m_columns.m_col_name);
        m_ipod_view.append_column(*pColIpod);
        m_ipod_scrolled_window.add(m_ipod_view);
        m_ipod_model->set_sort_column(m_columns.m_col_name, Gtk::SORT_ASCENDING);
        m_ipod_view.get_selection()->set_mode(Gtk::SELECTION_MULTIPLE);
        m_ipod_view.signal_button_press_event().connect(sigc::mem_fun(*this, &PlayerWindow::on_ipod_button_press), false);
        m_ipod_view.signal_row_activated().connect(sigc::mem_fun(*this, &PlayerWindow::on_ipod_row_activated));
        m_ipod_view.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &PlayerWindow::on_ipod_selected));
        m_middle_pane.pack2(m_ipod_scrolled_window, true, false);
        m_middle_pane.set_position(400); // Divisione iniziale

        // Setup Drag and Drop Destinazione (iPod View)
        std::vector<Gtk::TargetEntry> destTargets;
        destTargets.push_back(Gtk::TargetEntry("text/plain"));
        m_ipod_view.enable_model_drag_dest(destTargets, Gdk::ACTION_COPY);
        m_ipod_view.signal_drag_data_received().connect(sigc::mem_fun(*this, &PlayerWindow::on_ipod_drag_data_received));

        // --- Colonna DB ---
        m_db_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_db_scrolled_window.add(m_db_view);
        m_db_model = Gtk::ListStore::create(m_columns);
        m_db_view.set_model(m_db_model);
        
        // Configurazione Colonne DB
        m_db_view.append_column("Artista", m_columns.m_col_artist);
        auto* col_artist = m_db_view.get_column(0);
        col_artist->set_sort_column(m_columns.m_col_artist);
        col_artist->set_resizable(true);
        col_artist->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_artist->set_fixed_width(50);
        col_artist->set_expand(true);

        m_db_view.append_column("Titolo", m_columns.m_col_title);
        auto* col_title = m_db_view.get_column(1);
        col_title->set_sort_column(m_columns.m_col_title);
        col_title->set_resizable(true);
        col_title->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_title->set_fixed_width(50);
        col_title->set_expand(true);

        m_db_view.append_column("Album", m_columns.m_col_album);
        auto* col_album = m_db_view.get_column(2);
        col_album->set_sort_column(m_columns.m_col_album);
        col_album->set_resizable(true);
        col_album->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_album->set_fixed_width(50);
        col_album->set_expand(true);

        m_db_view.append_column("Anno", m_columns.m_col_year);
        auto* col_year = m_db_view.get_column(3);
        col_year->set_sort_column(m_columns.m_col_year);
        col_year->set_resizable(true);
        col_year->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_year->set_fixed_width(50);
        col_year->set_expand(true);

        m_db_view.append_column("Genere", m_columns.m_col_genre);
        auto* col_genre = m_db_view.get_column(4);
        col_genre->set_sort_column(m_columns.m_col_genre);
        col_genre->set_resizable(true);
        col_genre->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_genre->set_fixed_width(50);
        col_genre->set_expand(true);

        m_db_view.append_column("Directory", m_columns.m_col_path);
        auto* col_path = m_db_view.get_column(5);
        col_path->set_sort_column(m_columns.m_col_path);
        col_path->set_resizable(true);
        col_path->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
        col_path->set_fixed_width(50);
        col_path->set_expand(true);

        m_db_view.signal_row_activated().connect(sigc::mem_fun(*this, &PlayerWindow::on_db_row_activated));
        m_db_view.get_selection()->set_mode(Gtk::SELECTION_MULTIPLE); // Abilita selezione multipla
        m_db_model->set_sort_column(m_columns.m_col_artist, Gtk::SORT_ASCENDING);
        m_db_model->signal_sort_column_changed().connect(sigc::mem_fun(*this, &PlayerWindow::on_db_sort_changed));



        m_left_container.pack_start(m_folder_file_pane, Gtk::PACK_EXPAND_WIDGET);
        m_left_container.pack_start(m_db_scrolled_window, Gtk::PACK_EXPAND_WIDGET);


        // --- Colonna 3: Placeholder ---
        m_col3_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_col3_scrolled_window.set_size_request(320, -1); // Minima larghezza per i controlli
        m_col3_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_col3_box.set_spacing(10);
        m_col3_scrolled_window.add(m_col3_box);

        m_audio_controls_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_audio_controls_box.set_spacing(5);
        
        // --- Metadata Controls ---
        m_info_hbox.set_orientation(Gtk::ORIENTATION_VERTICAL);
        m_info_hbox.set_spacing(10);

        m_metadata_grid.set_row_spacing(5);
        m_metadata_grid.set_column_spacing(5);
        m_metadata_grid.set_margin_top(10);

        m_lbl_artist.set_text("Artista:");
        m_lbl_title.set_text("Titolo:");
        m_lbl_album.set_text("Album:");
        m_lbl_year.set_text("Anno:");
        m_lbl_genre.set_text("Genere:");

        m_metadata_grid.attach(m_lbl_artist, 0, 0, 1, 1);
        m_metadata_grid.attach(m_entry_artist, 1, 0, 1, 1);
        m_metadata_grid.attach(m_lbl_title, 0, 1, 1, 1);
        m_metadata_grid.attach(m_entry_title, 1, 1, 1, 1);
        m_metadata_grid.attach(m_lbl_album, 0, 2, 1, 1);
        m_metadata_grid.attach(m_entry_album, 1, 2, 1, 1);
        m_metadata_grid.attach(m_lbl_year, 0, 3, 1, 1);
        m_metadata_grid.attach(m_entry_year, 1, 3, 1, 1);
        m_metadata_grid.attach(m_lbl_genre, 0, 4, 1, 1);
        m_metadata_grid.attach(m_entry_genre, 1, 4, 1, 1);

        // Etichette per visualizzare i valori nel DB (solo lettura)
        auto set_db_lbl_style = [](Gtk::Label& lbl) {
            lbl.set_halign(Gtk::ALIGN_START);
            lbl.set_ellipsize(Pango::ELLIPSIZE_END);
            lbl.set_opacity(0.6); // Leggermente sbiadito per distinguerlo
        };
        set_db_lbl_style(m_lbl_db_artist); m_metadata_grid.attach(m_lbl_db_artist, 2, 0, 1, 1);
        set_db_lbl_style(m_lbl_db_title);  m_metadata_grid.attach(m_lbl_db_title, 2, 1, 1, 1);
        set_db_lbl_style(m_lbl_db_album);  m_metadata_grid.attach(m_lbl_db_album, 2, 2, 1, 1);
        set_db_lbl_style(m_lbl_db_year);   m_metadata_grid.attach(m_lbl_db_year, 2, 3, 1, 1);
        set_db_lbl_style(m_lbl_db_genre);  m_metadata_grid.attach(m_lbl_db_genre, 2, 4, 1, 1);
        m_metadata_grid.set_column_spacing(10); // Spazio tra campi input e valori DB

        m_btn_search_online.set_label("Cerca Online");
        m_btn_search_online.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_search_online_clicked));
        m_metadata_grid.attach(m_btn_search_online, 0, 5, 2, 1);

        m_save_buttons_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_save_buttons_box.set_spacing(5);

        m_btn_save_metadata.set_label("Salva Metadati");
        m_btn_save_metadata.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_save_metadata_clicked));
        m_save_buttons_box.pack_start(m_btn_save_metadata, Gtk::PACK_EXPAND_WIDGET);

        m_btn_save_db_only.set_label("Salva solo DB");
        m_btn_save_db_only.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_save_db_only_clicked));
        m_save_buttons_box.pack_start(m_btn_save_db_only, Gtk::PACK_EXPAND_WIDGET);
        
        m_metadata_grid.attach(m_save_buttons_box, 0, 6, 2, 1);

        m_btn_recognize.set_label("Invia a SongRep");
        m_btn_recognize.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_recognize_clicked));
        m_metadata_grid.attach(m_btn_recognize, 0, 7, 2, 1);

        m_info_hbox.pack_start(m_metadata_grid, Gtk::PACK_SHRINK);

        // --- Technical Info Grid ---
        m_tech_grid.set_row_spacing(5);
        m_tech_grid.set_column_spacing(10);
        m_tech_grid.set_margin_top(10);

        m_lbl_bitrate.set_text("Bitrate:"); m_tech_grid.attach(m_lbl_bitrate, 0, 0, 1, 1);
        m_val_bitrate.set_halign(Gtk::ALIGN_START); m_tech_grid.attach(m_val_bitrate, 1, 0, 1, 1);

        m_lbl_codec.set_text("Codec:"); m_tech_grid.attach(m_lbl_codec, 0, 1, 1, 1);
        m_val_codec.set_halign(Gtk::ALIGN_START); m_tech_grid.attach(m_val_codec, 1, 1, 1, 1);

        m_lbl_samplerate.set_text("Sample Rate:"); m_tech_grid.attach(m_lbl_samplerate, 0, 2, 1, 1);
        m_val_samplerate.set_halign(Gtk::ALIGN_START); m_tech_grid.attach(m_val_samplerate, 1, 2, 1, 1);

        m_lbl_size.set_text("Dimensione:"); m_tech_grid.attach(m_lbl_size, 0, 3, 1, 1);
        m_val_size.set_halign(Gtk::ALIGN_START); m_tech_grid.attach(m_val_size, 1, 3, 1, 1);

        m_info_hbox.pack_start(m_tech_grid, Gtk::PACK_SHRINK);

        m_audio_controls_box.pack_start(m_info_hbox, Gtk::PACK_SHRINK);

        m_btn_save_cover.set_label("Salva Copertina");
        m_btn_save_cover.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_save_cover_clicked));
        m_col3_box.pack_start(m_btn_save_cover, Gtk::PACK_SHRINK);

        // Controlli Playback spostati qui
        m_btn_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
        m_btn_box.set_spacing(5);
        m_btn_box.set_halign(Gtk::ALIGN_CENTER);
        m_btn_box.pack_start(m_btn_play, Gtk::PACK_SHRINK);
        m_btn_box.pack_start(m_btn_pause, Gtk::PACK_SHRINK);
        m_btn_box.pack_start(m_btn_stop, Gtk::PACK_SHRINK);
        m_col3_box.pack_start(m_btn_box, Gtk::PACK_SHRINK);
        m_col3_box.pack_start(m_slider_box, Gtk::PACK_SHRINK);

        m_col3_box.pack_start(m_audio_controls_box, Gtk::PACK_SHRINK);
        m_image_preview.set_halign(Gtk::ALIGN_END);
        m_col3_box.pack_start(m_image_preview, Gtk::PACK_SHRINK);
        
        m_image_preview.signal_button_press_event().connect(sigc::mem_fun(*this, &PlayerWindow::on_image_clicked), false);
        m_text_view.set_editable(false);
        m_text_view.set_wrap_mode(Gtk::WRAP_WORD);
        m_col3_box.pack_start(m_text_view, Gtk::PACK_EXPAND_WIDGET);        

        m_main_pane.pack2(m_col3_scrolled_window, true, false);

        show_all_children();
        m_scan_progress_bar.hide();
        m_db_scrolled_window.hide();
        m_audio_controls_box.hide(); // Nascondi controlli audio all'avvio
        m_btn_save_db_only.hide(); // Nascondi pulsante DB all'avvio (siamo in vista file)

        // Controlla la config all'avvio
        if (!loadConfig(m_config)) {
            Glib::signal_timeout().connect([this]() { on_config_clicked(); return false; }, 200);
        } else {
            Glib::signal_timeout().connect([this]() { refresh_list(); return false; }, 200);
        }

        signal_realize().connect([this](){ m_main_pane.set_position(get_width() * 2 / 3); });

        m_col3_box.show_all();
        
        load_meta_cache();

        Glib::signal_timeout().connect(sigc::mem_fun(*this, &PlayerWindow::on_update_position), 500);

        // Setup MIDI Dispatchers per thread-safety
        m_dispatcher_play.connect(sigc::mem_fun(*this, &PlayerWindow::on_play_clicked));
        m_dispatcher_pause.connect(sigc::mem_fun(*this, &PlayerWindow::on_pause_clicked));
        m_dispatcher_stop.connect(sigc::mem_fun(*this, &PlayerWindow::on_stop_clicked));
        m_dispatcher_next.connect(sigc::mem_fun(*this, &PlayerWindow::on_next_track));

        // Avvia il thread di ascolto MIDI per Hercules
        m_midi_thread = std::thread(&PlayerWindow::midi_listener_thread, this);
    }

    ~PlayerWindow() override {
        m_stop_scan = true;
        if (m_scan_thread.joinable()) m_scan_thread.join();
        
        m_stop_midi = true;
        if (m_midi_thread.joinable()) m_midi_thread.join();
        
        save_meta_cache();
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
        }
        if (m_db) {
            sqlite3_close(m_db);
        }
    }

    void midi_listener_thread() {
        snd_seq_t *seq;
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
            LOG(ERROR, "Could not open ALSA sequencer. MIDI support disabled.");
            return;
        }
        
        snd_seq_set_client_name(seq, "MusicPlayer-MIDI-Listener"); // Nome del client MIDI
        int my_port = snd_seq_create_simple_port(seq, "Input", // Creazione di una porta MIDI semplice
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC);

        LOG(INFO, "MIDI Listener started as client " << snd_seq_client_id(seq));

        // --- Auto-connessione alla Hercules DJCONTROL MIX ---
        snd_seq_client_info_t *cinfo;
        snd_seq_client_info_alloca(&cinfo);
        snd_seq_client_info_set_client(cinfo, -1);
        
        bool connected = false;
        while (snd_seq_query_next_client(seq, cinfo) >= 0) {
            std::string name = snd_seq_client_info_get_name(cinfo);
            if (name.find("DJCONTROL MIX") != std::string::npos) {
                int hercules_id = snd_seq_client_info_get_client(cinfo);
                
                snd_seq_port_info_t *pinfo;
                snd_seq_port_info_alloca(&pinfo);
                snd_seq_port_info_set_client(pinfo, hercules_id);
                snd_seq_port_info_set_port(pinfo, -1);
                
                if (snd_seq_query_next_port(seq, pinfo) >= 0) {
                    int hercules_port = snd_seq_port_info_get_port(pinfo);
                    if (snd_seq_connect_from(seq, my_port, hercules_id, hercules_port) == 0) {
                        LOG(INFO, "Successfully auto-connected to " << name << " (Client " << hercules_id << ")");
                        connected = true;
                        break;
                    }
                }
            }
        }
        
        if (!connected) {
            LOG(WARNING, "Hercules DJCONTROL MIX not found or could not connect. Please check cables.");
        } else {
            LOG(INFO, "MIDI Ready. Press buttons on your Hercules!");
        }

        snd_seq_event_t *ev;
        while (!m_stop_midi) {
            if (snd_seq_event_input_pending(seq, 1) > 0) {
                snd_seq_event_input(seq, &ev);
                
                // Mapping codici Hercules (Esempio standard DJControl Inpulse)
                if (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity == 0) {
                    snd_seq_free_event(ev);
                    continue; // Ignora il rilascio del tasto
                }

                if (ev->type == SND_SEQ_EVENT_NOTEON) {
                    int note = ev->data.note.note;
                    LOG(DEBUG, "MIDI Note: " << note);
                    switch(note) {
                        case 7: { // Tasto Play/Pause della DJCONTROL MIX
                            GstState state = GST_STATE_NULL;
                            GstState pending = GST_STATE_NULL;
                            if (m_pipeline) {
                                gst_element_get_state(m_pipeline, &state, &pending, 0);
                            }
                            // Se sta riproducendo o sta per farlo, mettiamo in pausa
                            if (state == GST_STATE_PLAYING || pending == GST_STATE_PLAYING) {
                                m_dispatcher_pause.emit();
                            } else {
                                m_dispatcher_play.emit();
                            }
                            break;
                        }
                        case 12: // Tipico tasto Play Deck A
                            m_dispatcher_play.emit();
                            break;
                        case 11: // Tipico tasto Cue/Pause
                            m_dispatcher_pause.emit();
                            break;
                        case 14: // Tasto Sync o Next
                            m_dispatcher_next.emit();
                            break;
                    }
                } 
                else if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
                    int param = ev->data.control.param;
                    int value = ev->data.control.value; // 0-127
                    LOG(DEBUG, "MIDI CC: " << param << " Value: " << value);

                    // Mapping per DJCONTROL MIX: 3 (Master), 8 (Deck 1), 9 (Deck 2)
                    if (param == 7 || param == 23 || param == 3 || param == 8 || param == 9) { 
                        double vol = (double)value / 127.0 * 100.0;
                        // Collega l'aggiornamento del volume con una priorità più alta
                        // per una maggiore reattività.
                        Glib::signal_idle().connect(
                            sigc::bind(sigc::mem_fun(*this, &PlayerWindow::set_volume_scale_value_from_midi), vol),
                            Glib::PRIORITY_HIGH_IDLE
                        );
                    }
                    else if (param == 33) { // Jog Wheel (Esempio seeking)
                        if (m_pipeline) {
                            Glib::signal_idle().connect([this, value]() {
                                double current = m_scale.get_value();
                                // Determina direzione dalla variazione (logica semplificata)
                                if (value > 64) m_scale.set_value(current + 2);
                                else m_scale.set_value(current - 2);
                                return false;
                            });
                        }
                    }
                    else if (param == 10) { // Jog Wheel FWD/REV (DJCONTROL MIX)
                        if (m_pipeline) {
                            Glib::signal_idle().connect([this, value]() {
                                double current = m_scale.get_value();
                                // Value 1: Rotazione Oraria (FWD+), Value 127: Rotazione Antioraria (REV-)
                                if (value == 1) m_scale.set_value(current + 2);
                                else if (value == 127) m_scale.set_value(current - 2);
                                return false;
                            });
                        }
                    }
                }
                snd_seq_free_event(ev);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        snd_seq_close(seq);
    }

    // Funzione helper per aggiornare il valore della scala del volume dal thread MIDI
    // Viene chiamata tramite Glib::signal_idle con alta priorità.
    bool set_volume_scale_value_from_midi(double vol_value) {
        m_volume_scale.set_value(vol_value);
        return false; // Indica che il callback deve essere eseguito una sola volta
    }

    void on_debug_toggle() {
        g_debug_enabled = m_btnDebugToggle.get_active();
        m_btnDebugToggle.set_label(g_debug_enabled ? "Debug On" : "Debug Off");
    }

    void on_db_view_toggled() {
        m_db_view_active = !m_db_view_active;
        if (m_db_view_active) {
            m_folder_file_pane.hide();
            m_db_scrolled_window.show();
            m_btn_save_db_only.show();
            m_btnDB.set_label("Cartelle");

            // Popola la vista DB dal database SQLite
            load_db_view();
        } else {
            m_db_scrolled_window.hide();
            m_folder_file_pane.show();
            m_btn_save_db_only.hide();
            m_btnDB.set_label("DB");
            m_scan_status_label.set_text("");
        }
    }

    void on_cast_clicked() {
        // Verifica preliminare del plugin Chromecast
        GstRegistry *registry = gst_registry_get();
        GstPlugin *plugin = gst_registry_find_plugin(registry, "chromecast");
        if (!plugin) {
            std::string error_msg = "Il plugin 'chromecast' non è disponibile.\n\n";
            
            // Verifica se il file esiste fisicamente (percorso standard Ubuntu x86_64)
            std::string plugin_path = "/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstchromecast.so";
            if (fs::exists(plugin_path)) {
                error_msg += "Il file del plugin ESISTE in: " + plugin_path + "\n";
                error_msg += "Tuttavia, GStreamer non riesce a caricarlo (Blacklist o dipendenze mancanti).\n\n";
                error_msg += "Soluzione:\n";
                error_msg += "1. Esegui nel terminale: gst-inspect-1.0 " + plugin_path + "\n";
                error_msg += "2. Installa le librerie mancanti indicate nell'errore.\n";
                error_msg += "3. Cancella cache: rm -rf ~/.cache/gstreamer-1.0";
            } else {
                error_msg += "Il file del plugin NON è stato trovato nel percorso standard.\n";
                error_msg += "Potrebbe essere in un percorso diverso o il pacchetto 'plugins-bad' è incompleto.\n\n";
                error_msg += "Soluzione:\n";
                error_msg += "1. Cerca il file: find /usr/lib -name libgstchromecast.so\n";
                error_msg += "2. Reinstalla: sudo apt install --reinstall gstreamer1.0-plugins-bad";
            }

            Gtk::MessageDialog warn(*this, "Errore Plugin Chromecast", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK);
            warn.set_secondary_text(error_msg);
            warn.run();
        } else {
            gst_object_unref(plugin);
        }

        Gtk::Dialog dlg("Seleziona Dispositivo Audio", *this, true);
        dlg.set_default_size(500, 300);
        
        Gtk::TreeView view;
        m_current_device_model = Gtk::ListStore::create(m_device_columns);
        view.set_model(m_current_device_model);
        view.append_column("Dispositivo", m_device_columns.m_col_name);
        view.append_column("Tipo", m_device_columns.m_col_class);
        
        Gtk::ScrolledWindow sw;
        sw.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        sw.add(view);
        dlg.get_content_area()->pack_start(sw, true, true, 10);
        
        dlg.add_button("Annulla", Gtk::RESPONSE_CANCEL);
        dlg.add_button("Connetti", Gtk::RESPONSE_OK);
        
        // Avvia monitoraggio dispositivi
        m_device_monitor = gst_device_monitor_new();
        GstBus *bus = gst_device_monitor_get_bus(m_device_monitor);
        gst_bus_add_watch(bus, (GstBusFunc)on_device_monitor_bus_msg_wrapper, this);
        gst_object_unref(bus);
        
        // Filtra per Audio Sinks (include Chromecast, DLNA, PulseAudio, ecc.)
        gst_device_monitor_add_filter(m_device_monitor, "Audio/Sink", NULL);
        gst_device_monitor_start(m_device_monitor);
        
        dlg.show_all_children();
        int result = dlg.run();
        
        if (result == Gtk::RESPONSE_OK) {
            auto sel = view.get_selection();
            auto iter = sel->get_selected();
            if (iter) {
                GstDevice* device = (*iter)[m_device_columns.m_col_device];
                set_sink_device(device);
            }
        }

        // Cleanup monitor
        gst_device_monitor_stop(m_device_monitor);
        gst_object_unref(m_device_monitor);
        m_device_monitor = nullptr;

        // Cleanup refs nei dispositivi trovati
        auto children = m_current_device_model->children();
        for (auto iter = children.begin(); iter != children.end(); ++iter) {
            GstDevice *d = (*iter)[m_device_columns.m_col_device];
            gst_object_unref(d);
        }
        m_current_device_model.reset();
    }

    void on_youtube_clicked() {
        std::string artist = m_entry_artist.get_text();
        std::string title = m_entry_title.get_text();
        std::string query;

        if (!artist.empty() && !title.empty()) {
            query = artist + " " + title;
        } else {
            auto selection = m_files_view.get_selection();
            auto iter = selection->get_selected();
            if (iter) {
                Gtk::TreeModel::Row row = *iter;
                Glib::ustring name = row[m_columns.m_col_name];
                size_t dot_pos = name.find_last_of('.');
                if (dot_pos != std::string::npos) name = name.substr(0, dot_pos);
                query = name;
            }
        }

        if (query.empty()) {
            Gtk::MessageDialog dlg(*this, "Nessuna informazione per la ricerca.", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK);
            dlg.run();
            return;
        }

        std::string url = "https://www.youtube.com/results?search_query=" + url_encode(query);
        
        try {
            Gio::AppInfo::launch_default_for_uri(url);
        } catch (const Glib::Error& ex) {
            LOG(ERROR, "Cannot open YouTube: " << ex.what());
            Gtk::MessageDialog dlg(*this, "Impossibile aprire il browser.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
            dlg.run();
        }
    }

    void set_sink_device(GstDevice* device) {
        if (!m_pipeline) return;
        GstElement *sink = gst_device_create_element(device, NULL);
        if (sink) {
            // Ferma la pipeline per cambiare il sink
            GstState current_state;
            gst_element_get_state(m_pipeline, &current_state, NULL, 0);
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            
            g_object_set(m_pipeline, "audio-sink", sink, NULL);
            
            if (current_state == GST_STATE_PLAYING || current_state == GST_STATE_PAUSED) {
                gst_element_set_state(m_pipeline, current_state);
            }
        }
    }

    void on_exit_clicked() {
        try {
            if (fs::exists("cache")) fs::remove_all("cache");
            if (fs::exists(".music_metadata_cache")) fs::remove(".music_metadata_cache");
        } catch(...) {}
        close();
    }

    void on_ipod_clicked() {
        Gtk::FileChooserDialog dialog(*this, "Seleziona Dispositivo iPod", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        dialog.add_button("Annulla", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Apri", Gtk::RESPONSE_OK);

        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_current_ipod_path = dialog.get_filename();
            load_ipod_content(m_current_ipod_path);
        }
    }

    void load_ipod_content(const std::string& path) {
        if (path.empty()) {
            m_ipod_model->clear();
            return;
        }
        m_ipod_model->clear();
        
        m_scan_status_label.set_text("Lettura iPod...");
        std::string ipod_path = path;

        std::thread([this, ipod_path]() {
            struct Item { std::string path; std::string filename; bool is_dir; };
            std::vector<Item> items_to_add;

            auto check_ext = [](const std::string& name) {
                std::string ext = fs::path(name).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                return (ext == ".mp3" || ext == ".m4a" || ext == ".wav" || ext == ".flac" || ext == ".ogg");
            };

            try {
                // 1. Scansione della root (canzoni aggiunte manualmente o file sparsi)
                for (const auto& entry : fs::directory_iterator(ipod_path)) {
                    std::string name = entry.path().filename().string();
                    if (name.empty() || name[0] == '.') continue;
                    if (entry.is_directory()) {
                        if (name != "iPod_Control") items_to_add.push_back({entry.path().string(), name, true});
                    } else if (check_ext(name)) {
                        items_to_add.push_back({entry.path().string(), name, false});
                    }
                }

                // 2. Scansione struttura standard iPod (F00, F01, ecc.)
                fs::path ipod_music_path = fs::path(ipod_path) / "iPod_Control" / "Music";
                if (fs::exists(ipod_music_path) && fs::is_directory(ipod_music_path)) {
                    for (const auto& entry : fs::directory_iterator(ipod_music_path)) {
                        if (entry.is_directory() && entry.path().filename().string().rfind("F", 0) == 0) {
                            for (const auto& music_entry : fs::directory_iterator(entry.path())) {
                                std::string name = music_entry.path().filename().string();
                                if (!name.empty() && name[0] != '.' && check_ext(name)) items_to_add.push_back({music_entry.path().string(), name, false});
                            }
                        }
                    }
                }

                for (const auto& item : items_to_add) {
                    std::string display_name = item.filename;
                    if (!item.is_dir) {
                        Metadata meta = extract_metadata_internal(item.path);
                        if (!meta.title.empty()) {
                            display_name = (meta.artist.empty() ? "" : meta.artist + " - ") + meta.title;
                        }
                    }

                    Glib::signal_idle().connect([this, display_name, item]() {
                        Gtk::TreeModel::Row row = *(m_ipod_model->append());
                        set_row_style(row, item.filename, item.is_dir); // Icona basata sul file reale
                        row[m_columns.m_col_name] = sanitize_utf8(display_name);      // Testo basato sui metadati
                        row[m_columns.m_col_path] = item.path;
                        return false;
                    });
                }
            } catch (const std::exception& e) {
                LOG(ERROR, "Errore iPod: " << e.what());
            }

            Glib::signal_idle().connect([this]() { m_scan_status_label.set_text(""); return false; });
        }).detach();
    }

    void on_ipod_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
        auto iter = m_ipod_model->get_iter(path);
        if (iter) play_track(iter, m_ipod_model);
    }

    void on_ipod_selected() {
        on_item_selected(m_ipod_view.get_selection());
    }

    void on_files_drag_data_get(const Glib::RefPtr<Gdk::DragContext>&, Gtk::SelectionData& selection_data, guint, guint) {
        auto iter = m_files_view.get_selection()->get_selected();
        if (iter) {
            Glib::ustring path = (*iter)[m_columns.m_col_path];
            selection_data.set_text(path);
        }
    }

    void on_ipod_drag_data_received(const Glib::RefPtr<Gdk::DragContext>& context, int, int, const Gtk::SelectionData& selection_data, guint, guint time) {
        Glib::ustring smb_path = selection_data.get_text();
        
        if (smb_path.empty() || m_current_ipod_path.empty()) {
            context->drag_finish(false, false, time);
            return;
        }

        std::string smb_path_str = (std::string)smb_path;
        std::string filename = fs::path(smb_path_str).filename().string();
        
        // Destinazione: se è un iPod reale, mettiamo in F00 per compatibilità hardware
        std::string local_dest;
        fs::path ipod_f00 = fs::path(m_current_ipod_path) / "iPod_Control" / "Music" / "F00";
        if (fs::exists(ipod_f00)) {
            local_dest = (ipod_f00 / filename).string();
        } else {
            local_dest = m_current_ipod_path + "/" + filename;
        }

        // Avviamo il download in un thread per non bloccare la UI durante la copia SMB
        m_scan_status_label.set_text("Copia in corso: " + filename + "...");
        
        std::thread([this, smb_path, local_dest, context, time]() {
            bool success = download_smb_file(smb_path, local_dest, m_config);
            bool is_ipod_shuffle = fs::exists(fs::path(m_current_ipod_path) / "iPod_Control");
            
            Glib::signal_idle().connect([this, success, context, time, is_ipod_shuffle]() {
                context->drag_finish(success, false, time);
                if (success) {
                    m_scan_status_label.set_text("Copia completata.");
                    if (is_ipod_shuffle) {
                        Gtk::MessageDialog note(*this, "File copiato sull'iPod", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                        note.set_secondary_text("L'iPod Shuffle richiede l'aggiornamento del database interno per riprodurre i brani senza PC.\n\n"
                                                "Puoi ascoltare i brani tramite questa app, ma per l'uso standalone dovrai usare iTunes o uno strumento come 'Rhythmbox' (con il plugin iPod abilitato) per sincronizzare il database.");
                        note.run();
                    }
                    load_ipod_content(m_current_ipod_path);
                } else {
                    m_scan_status_label.set_text("Errore durante la copia.");
                    Gtk::MessageDialog dlg(*this, "Errore nella copia del file sul dispositivo.", 
                                         false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                    dlg.run();
                }
                return false;
            });
        }).detach();
    }

    void on_config_clicked() {
        LOG(INFO, "Config button clicked");
        ConfigDialog dlg(*this, m_config);
        if (dlg.run() == Gtk::RESPONSE_OK) {
            saveConfig(m_config);
            refresh_list();
        }
    }

    void on_folder_selected() {
        LOG(INFO, "Folder selected");
        auto refSelection = m_folder_view.get_selection();
        auto iter = refSelection->get_selected();
        if(iter) {
            Gtk::TreeModel::Row row = *iter;
            Glib::ustring path = row[m_columns.m_col_path];
            if (path.empty()) return;

            int gen = ++m_folder_selection_gen;
            SambaConfig config = m_config;

            std::thread([this, path, config, gen](){
                std::string output = exec_smb(path, config);
                
                // Parsing nel thread secondario
                std::vector<std::pair<std::string, bool>> entries;
                std::istringstream stream(output);
                std::string line;
                std::string name;
                bool is_folder;
                while (std::getline(stream, line)) {
                    bool is_hidden;
                    if (parse_smb_line(line, name, is_folder, is_hidden)) {
                        entries.push_back({name, is_folder});
                    }
                }

                Glib::signal_idle().connect([this, entries, gen, path](){
                    if (m_folder_selection_gen != gen) return false;
                    
                    // Scollega il modello per velocizzare l'inserimento
                    m_files_view.set_model(Glib::RefPtr<Gtk::ListStore>());
                    m_files_model->clear();
                    
                    for (const auto& entry : entries) {
                        Gtk::TreeModel::Row row = *(m_files_model->append());
                        set_row_style(row, entry.first, entry.second);
                        if (!entry.second) {
                            std::string full_path = (std::string)path;
                            if (!full_path.empty() && full_path.back() != '/') full_path += "/";
                            full_path += entry.first;
                            row[m_columns.m_col_path] = full_path;
                        }
                    }
                    
                    // Ricollega il modello
                    m_files_view.set_model(m_files_model);
                    m_pColFile->set_title("File (" + std::to_string(m_files_model->children().size()) + ")");
                    return false;
                });
            }).detach();
        }
    }

    void load_folder_content(const Gtk::TreeModel::iterator& iter) {
        Gtk::TreeModel::Row row = *iter;
        // Pulisci i figli esistenti
        while(!row.children().empty()) m_folder_model->erase(row.children().begin());

        // Aggiungi placeholder "Loading..."
        Gtk::TreeModel::Row child = *(m_folder_model->append(row.children()));
        child[m_columns.m_col_name] = "Loading...";

        Glib::ustring path = row[m_columns.m_col_path];
        Gtk::TreeRowReference row_ref(m_folder_model, m_folder_model->get_path(iter));
        SambaConfig config = m_config;

        std::thread([this, path, config, row_ref](){
            std::string output = exec_smb(path, config);
            Glib::signal_idle().connect([this, output, row_ref, path](){
                if (!row_ref.is_valid()) return false;
                auto iter = m_folder_model->get_iter(row_ref.get_path());
                if (!iter) return false;
                Gtk::TreeModel::Row row = *iter;
                
                // Rimuovi "Loading..."
                while(!row.children().empty()) m_folder_model->erase(row.children().begin());

                std::istringstream stream(output);
                std::string line;
                while (std::getline(stream, line)) {
                    std::string name;
                    bool is_folder = false;
                    bool is_hidden = false;
                    if (parse_smb_line(line, name, is_folder, is_hidden) && is_folder) {
                            Gtk::TreeModel::Row new_row = *(m_folder_model->append(row.children()));
                            new_row[m_columns.m_col_name] = name;
                            new_row[m_columns.m_col_icon] = "folder";
                            
                            std::string sub_path = path;
                            if (sub_path.back() != '/') sub_path += "/";
                            sub_path += name;
                            new_row[m_columns.m_col_path] = sub_path;

                            Gtk::TreeModel::Row placeholder = *(m_folder_model->append(new_row.children()));
                            placeholder[m_columns.m_col_name] = "Loading...";
                    }
                }
                return false;
            });
        }).detach();
    }

    void on_folder_expanded(const Gtk::TreeModel::iterator& iter, const Gtk::TreeModel::Path& /*path*/) {
        Gtk::TreeModel::Row row = *iter;
        if (row.children().empty()) return;

        auto child_iter = row.children().begin();
        Glib::ustring child_name = (*child_iter)[m_columns.m_col_name];

        if (child_name == "Loading...") {
            load_folder_content(iter);
        }
    }

    void on_file_selected() {
        on_item_selected(m_files_view.get_selection());
    }

    void on_item_selected(Glib::RefPtr<Gtk::TreeSelection> selection) {
        auto iter = selection->get_selected();
        
        // Reset UI immediately
        m_image_preview.clear();
        m_image_preview.hide();
        m_text_view.get_buffer()->set_text("");
        m_text_view.hide();
        m_audio_controls_box.hide();
        m_entry_artist.set_text("");
        m_entry_title.set_text("");
        m_entry_album.set_text("");
        m_entry_year.set_text("");
        m_entry_genre.set_text("");
        m_val_bitrate.set_text("");
        m_val_codec.set_text("");
        m_val_samplerate.set_text("");
        m_val_size.set_text("");

        m_lbl_db_artist.set_text("");
        m_lbl_db_title.set_text("");
        m_lbl_db_album.set_text("");
        m_lbl_db_year.set_text("");
        m_lbl_db_genre.set_text("");

        // Pulisci eventuale copertina temporanea precedente
        if (fs::exists(get_cache_dir() + "/temp_cover.jpg")) {
            fs::remove("/tmp/temp_cover.jpg");
        }

        if (iter) {
            Gtk::TreeModel::Row row = *iter;
            Glib::ustring name = row[m_columns.m_col_name];
            Glib::ustring path = row[m_columns.m_col_path];
            
            if (path.empty()) return;
            std::string path_str = (std::string)path;
            
            int gen = ++m_selection_gen;

            std::string ext = "";
            size_t dot_pos = path_str.find_last_of('.');
            if (dot_pos != std::string::npos) ext = path_str.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" || ext == ".bmp") {
                if (path_str.find("//") != 0 && path_str.find("smb:") != 0) { // Locale (iPod)
                    display_image(path_str);
                    return;
                }
                // Remoto (Samba)
                std::string cache_path = get_hashed_path(path, ext);
                if (fs::exists(cache_path)) {
                    display_image(cache_path);
                } else {
                    std::thread([this, path_str, cache_path, gen]() {
                        if (download_smb_file(path_str, cache_path, m_config)) {
                            Glib::signal_idle().connect([this, cache_path, gen]() {
                                if (m_selection_gen == gen) display_image(cache_path);
                                return false;
                            });
                        }
                    }).detach();
                }
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                bool need_extract = true; 
                if (m_meta_cache.count(path_str)) {
                    Metadata meta = m_meta_cache[path_str];
                    if (meta.size_mb > 0.0) {
                        update_metadata_ui(meta);
                        need_extract = false;
                    }
                }
                if (need_extract) {
                    std::thread([this, path_str, ext, gen]() {
                        std::string process_path;
                        bool is_remote = (path_str.find("//") == 0 || path_str.find("smb:") == 0);

                        if (is_remote) {
                            process_path = "/tmp/metadata_preview_" + std::to_string(gen) + ext;
                            if (!download_smb_file(path_str, process_path, m_config)) return;
                        } else {
                            process_path = path_str;
                        }

                        Metadata meta = extract_metadata_internal(process_path);
                        
                        Glib::signal_idle().connect([this, meta, path_str, gen, is_remote, process_path]() {
                            if (m_selection_gen == gen) {
                                Metadata final_meta = meta;
                                // Se remoto, salviamo la cover estratta nella cache locale
                                if (is_remote && !final_meta.coverPath.empty() && fs::exists(final_meta.coverPath)) {
                                    std::string cache_cover = get_hashed_path(path_str, ".jpg");
                                    fs::copy_file(final_meta.coverPath, cache_cover, fs::copy_options::overwrite_existing);
                                    fs::remove(final_meta.coverPath);
                                    final_meta.coverPath = cache_cover;
                                }
                                
                                update_metadata_ui(final_meta);
                                m_meta_cache[path_str] = final_meta;
                                update_db_labels(path_str);
                            }
                            if (is_remote && fs::exists(process_path)) fs::remove(process_path);
                            return false;
                        });
                    }).detach();
                }
            } else {
                if (path_str.find("//") != 0 && path_str.find("smb:") != 0) { display_text(path_str); return; }
                
                std::string cache_path = get_hashed_path(path_str, ext);
                if (fs::exists(cache_path)) {
                    display_text(cache_path);
                } else {
                    std::thread([this, path_str, cache_path, gen]() {
                        if (download_smb_file(path_str, cache_path, m_config)) {
                            Glib::signal_idle().connect([this, cache_path, gen]() {
                                if (m_selection_gen == gen) {
                                    display_text(cache_path);
                                }
                                return false;
                            });
                        }
                    }).detach();
                }
            }
        }
    }

    void display_image(const std::string& path) {
        try { 
            auto pixbuf = Gdk::Pixbuf::create_from_file(path);
            int width = m_col3_scrolled_window.get_allocation().get_width();
            if (width <= 20) width = 300; // Fallback se allocazione non ancora avvenuta
            if (width > 20) {
                int new_width = width - 20;
                int new_height = (pixbuf->get_height() * new_width) / pixbuf->get_width();
                pixbuf = pixbuf->scale_simple(new_width, new_height, Gdk::INTERP_BILINEAR);
            }
            m_image_preview.set(pixbuf); 
            m_image_preview.show();
        } catch(...) {}
    }

    void on_save_cover_clicked() {
        auto selection = m_files_view.get_selection();
        auto iter = selection->get_selected();
       if (!iter) return;

        Gtk::TreeModel::Row row = *iter;
        Glib::ustring song_path = row[m_columns.m_col_path];

        std::string source_path = get_cache_dir() + "/temp_cover.jpg";
        LOG(INFO, "Source path: " << source_path);
        if (fs::exists(source_path)) {
            std::string destination_path = fs::path(song_path).parent_path().string() + "/cover.jpg";
            LOG(INFO, "Destination path: " << destination_path);
            try {
                fs::copy_file(source_path, destination_path, fs::copy_options::overwrite_existing);
                Gtk::MessageDialog dlg(*this, "Copertina salvata in " + destination_path, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                dlg.run();
            } catch (const std::exception& e) {
               std::string error_message = "Errore durante il salvataggio della copertina: " + std::string(e.what()) +
                                             "\nSource Path: " + source_path +
                                             "\nDestination Path: " + destination_path;

                LOG(ERROR, error_message);

                Gtk::MessageDialog dlg(
                    *this, error_message,
                    false,
                    Gtk::MESSAGE_ERROR,
                    Gtk::BUTTONS_OK);

                dlg.run();
            }
        } else {
            Gtk::MessageDialog dlg(*this, "Nessuna copertina da salvare.", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK);
            dlg.run();
        }
    }
    bool on_image_clicked(GdkEventButton* event) {
        LOG(INFO, "Image clicked event received"); // Debug message
        if (event->type == GDK_BUTTON_PRESS && event->button == 1) { // Tasto sinistro
            LOG(INFO, "Left mouse button pressed"); // Debug message
            std::string source_path = "/tmp/temp_cover.jpg";
            if (fs::exists(source_path)) {
                std::string destination_path = fs::current_path().string() + "/cover.jpg";
                fs::copy_file(source_path, destination_path, fs::copy_options::overwrite_existing);                
            }
        }
        return false;
    }
    void display_text(const std::string& path) {
        try { 
            std::ifstream file(path);
            std::stringstream buffer;
            buffer << file.rdbuf();
            m_text_view.get_buffer()->set_text(sanitize_utf8(buffer.str()));
            m_text_view.show();
        } catch(...) {}
    }

    void update_metadata_ui(const Metadata& meta) {
        m_audio_controls_box.show();
        m_info_hbox.show_all();
        m_entry_artist.set_text(sanitize_utf8(meta.artist));
        m_entry_title.set_text(sanitize_utf8(meta.title));
        m_entry_album.set_text(sanitize_utf8(meta.album));
        m_entry_year.set_text(sanitize_utf8(meta.year));
        m_entry_genre.set_text(sanitize_utf8(meta.genre));

        if (!meta.artist.empty() && !meta.title.empty()) {
            set_title(meta.artist + " - " + meta.title);
        }

        if (!meta.coverPath.empty() && fs::exists(meta.coverPath)) {
            display_image(meta.coverPath);
        }

        std::stringstream ss;
        if (meta.bitrate > 0) ss << (meta.bitrate / 1000) << " kbps";
        else ss << "-";
        m_val_bitrate.set_text(ss.str());
        ss.str(""); ss.clear();

        m_val_codec.set_text(meta.codec.empty() ? "-" : meta.codec);

        if (meta.samplerate > 0) ss << meta.samplerate << " Hz";
        else ss << "-";
        m_val_samplerate.set_text(ss.str());
        ss.str(""); ss.clear();

        ss << std::fixed << std::setprecision(2) << meta.size_mb << " MB";
        m_val_size.set_text(ss.str());
    }

    std::string get_cache_dir() {
        std::string dir = "cache";
        if (!fs::exists(dir)) fs::create_directory(dir);
        return dir;
    }

    std::string get_hashed_path(const std::string& path, const std::string& ext) {
        size_t hash = std::hash<std::string>{}(path);
        return get_cache_dir() + "/" + std::to_string(hash) + ext;
    }

    void load_meta_cache() {
        std::ifstream f(".music_metadata_cache");
        std::string line;
        while(std::getline(f, line)) {
            std::istringstream iss(line);
            std::string path, artist, title, album, year, genre, coverPath, bitrate_s, codec, samplerate_s, size_s;
            auto read_field = [&](std::string& field) {
                std::getline(iss, field, '|');
            };
            read_field(path);
            read_field(artist);
            read_field(title);
            read_field(album);
            read_field(year);
            read_field(genre);
            read_field(coverPath);
            read_field(bitrate_s);
            read_field(codec);
            read_field(samplerate_s);
            read_field(size_s);

            if (!path.empty()) {
                Metadata m = {artist, title, album, year, genre, "", coverPath};
                if (!bitrate_s.empty()) m.bitrate = std::stoul(bitrate_s);
                m.codec = codec;
                if (!samplerate_s.empty()) m.samplerate = std::stoul(samplerate_s);
                if (!size_s.empty()) m.size_mb = std::stod(size_s);
                m_meta_cache[path] = m;
            }
        }
    }

    void save_meta_cache() {
        std::ofstream f(".music_metadata_cache");
        for (const auto& [path, meta] : m_meta_cache) {
            f << path << "|" << meta.artist << "|" << meta.title << "|" 
              << meta.album << "|" << meta.year << "|" << meta.genre << "|"
              << meta.coverPath << "|" << meta.bitrate << "|" << meta.codec << "|"
              << meta.samplerate << "|" << meta.size_mb << "\n";
        }
    }

    void on_file_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
        auto iter = m_files_model->get_iter(path);
        if (!iter) return;
        Gtk::TreeModel::Row row = *iter;
        
        Glib::ustring icon = row[m_columns.m_col_icon];
        if (icon == "folder") {
            Glib::ustring name = row[m_columns.m_col_name];
            auto folder_sel = m_folder_view.get_selection();
            auto folder_iter = folder_sel->get_selected();
            if (folder_iter) {
                m_folder_view.expand_row(m_folder_model->get_path(folder_iter), false);
                for (auto child : folder_iter->children()) {
                    if ((Glib::ustring)child[m_columns.m_col_name] == name) {
                        m_folder_view.get_selection()->select(child);
                        m_folder_view.scroll_to_row(m_folder_model->get_path(child));
                        return;
                    }
                }
            }
        } else {
            play_track(iter, m_files_model);
        }
    }

    bool on_update_position() {
        if (m_pipeline) {
            // Check for EOS (End Of Stream)
            GstBus *bus = gst_element_get_bus(m_pipeline);
            if (bus) {
                while (true) {
                    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 0, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_TAG));
                    if (!msg) break;

                    switch (GST_MESSAGE_TYPE(msg)) {
                        case GST_MESSAGE_EOS:
                            on_next_track();
                            break;
                        case GST_MESSAGE_ERROR:
                {
                    GError *err = NULL;
                    gchar *debug_info = NULL;
                    gst_message_parse_error(msg, &err, &debug_info);
                    LOG(ERROR, "GStreamer error received from element " << GST_OBJECT_NAME(msg->src) << ": " << err->message);
                    if (debug_info) {
                        LOG(ERROR, "Debugging info: " << debug_info);
                    }
                    g_clear_error(&err);
                    g_free(debug_info);
                    // Optionally, stop playback or show an error dialog
                    on_stop_clicked(); // Stop playback on error
                    Glib::signal_idle().connect([this, error_msg = std::string(err ? err->message : "Unknown GStreamer error")]() {
                        Gtk::MessageDialog dlg(*this, "Errore GStreamer", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                        dlg.set_secondary_text("Si è verificato un errore durante la riproduzione: " + error_msg);
                        dlg.run();
                        return false;
                    });
                    break;
                }
                        case GST_MESSAGE_TAG: {
                            GstTagList *tags = nullptr;
                            gst_message_parse_tag(msg, &tags);
                            update_metadata_from_tags(tags);
                            
                            // Aggiorna cache durante la riproduzione
                            if (m_current_track_row.is_valid()) {
                                auto model = m_current_track_row.get_model();
                                auto iter = model->get_iter(m_current_track_row.get_path());
                                if (iter) {
                                    Glib::ustring path = (*iter)[m_columns.m_col_path];
                                    Metadata meta;
                            meta.artist = sanitize_utf8(m_entry_artist.get_text());
                            meta.title = sanitize_utf8(m_entry_title.get_text());
                            meta.album = sanitize_utf8(m_entry_album.get_text());
                            meta.year = sanitize_utf8(m_entry_year.get_text());
                            meta.genre = sanitize_utf8(m_entry_genre.get_text());
                                    // Preserve tech info
                                    if (m_meta_cache.count((std::string)path)) {
                                        auto& old = m_meta_cache[(std::string)path];
                                        meta.bitrate = old.bitrate; meta.codec = old.codec; meta.samplerate = old.samplerate; meta.size_mb = old.size_mb;
                                    }
                                    m_meta_cache[(std::string)path] = meta;
                                }
                            }
                            gst_tag_list_unref(tags);
                            break;
                        }
                        default: break;
                    }
                    gst_message_unref(msg);
                }
                gst_object_unref(bus);
            }

            gint64 pos = 0;
            gint64 len = 0;
            if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos) &&
                gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &len)) {
                m_updating_scale = true;
                m_scale.set_range(0, len / GST_SECOND);
                m_scale.set_value(pos / GST_SECOND);
                m_updating_scale = false;
                
                m_time_label.set_text(format_time(pos / GST_SECOND) + " / " + format_time(len / GST_SECOND));
            }
        }
        GstState state = GST_STATE_NULL;
        if (m_pipeline) gst_element_get_state(m_pipeline, &state, NULL, 0);
        LOG(DEBUG, "on_update_position called. Pipeline state: " << state);
        return true;
    }

    void play_track(const Gtk::TreeModel::iterator& iter, Glib::RefPtr<Gtk::TreeModel> model) {
        if (!iter) return;
        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring path_val = row[m_columns.m_col_path];
        m_automatic_cover_search_done = false;
        set_title(name);
        LOG(DEBUG, "Attempting to play track: " << name << " from path: " << path_val);

        if (m_meta_cache.count((std::string)path_val)) {
            update_metadata_ui(m_meta_cache[(std::string)path_val]);
        } else {
            m_entry_artist.set_text("");
            m_entry_title.set_text("");
            m_entry_album.set_text("");
            m_entry_year.set_text("");
            m_entry_genre.set_text("");
            m_val_bitrate.set_text("");
            m_val_codec.set_text("");
            m_val_samplerate.set_text("");
            m_val_size.set_text("");
            LOG(DEBUG, "Metadata not in cache for: " << path_val << ", UI cleared.");
        }
        update_db_labels(path_val);

        std::string ext = "";
        size_t dot_pos = name.find_last_of('.');
        if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
        // Ensure ext starts with a dot if it's not empty
        if (!ext.empty() && ext[0] != '.') {
            ext = "." + ext;
        }
        // Fallback if no extension found, assume .mp3 for playback attempt
        if (ext.empty()) ext = ".mp3";

        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
            if (!m_pipeline) {
                m_pipeline = gst_element_factory_make("playbin", "playbin");
            } else {
                gst_element_set_state(m_pipeline, GST_STATE_NULL);
                LOG(DEBUG, "GStreamer pipeline reset to NULL state.");
            }

            if (m_pipeline) {
                if (!gst_is_initialized()) {
                    LOG(ERROR, "GStreamer is not initialized!");
                    return;
                }
                std::string path_str = (std::string)path_val;
                LOG(DEBUG, "Processing path: " << path_str);
                std::string play_path;
                bool ready = false;

                if (path_str.find("//") != std::string::npos || path_str.find("smb:") != std::string::npos) { // Percorso Samba
                    play_path = "/tmp/current_track" + ext;
                    if (download_smb_file(path_val, play_path, m_config)) ready = true;
                } else { // Percorso locale (iPod)
                    LOG(DEBUG, "Local path detected: " << path_str);
                    play_path = path_str;
                    if (fs::exists(play_path)) {
                        ready = true;
                        LOG(DEBUG, "Local file exists: " << play_path);
                    } else {
                        LOG(ERROR, "Local file does not exist or is not accessible: " << play_path);
                    }
                }
                
                if (ready) {
                    std::string uri = "file://" + play_path;
                    g_object_set(m_pipeline, "uri", uri.c_str(), NULL);
                    g_object_set(m_pipeline, "volume", m_volume_scale.get_value() / 100.0, NULL);
                    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                    
                    m_current_track_row = Gtk::TreeModel::RowReference(model, Gtk::TreeModel::Path(iter));
                    if (model == m_files_model) m_files_view.get_selection()->select(iter);
                    else if (model == m_ipod_model) m_ipod_view.get_selection()->select(iter);
                    else if (model == m_db_model) m_db_view.get_selection()->select(iter);
                } else {
                    LOG(ERROR, "Playback not ready for path: " << path_val << ". Check previous errors.");
                    Gtk::MessageDialog dlg(*this, "Errore Riproduzione", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                    dlg.set_secondary_text("Impossibile preparare il file per la riproduzione. Controlla i log per maggiori dettagli.");
                    dlg.run();
                }
            }
        }
    }

    void on_volume_changed() {
        if (m_pipeline) {
            g_object_set(m_pipeline, "volume", m_volume_scale.get_value() / 100.0, NULL);
        }
    }

    void on_next_track() {
        if (!m_current_track_row.is_valid()) return;
        auto model = m_current_track_row.get_model();
        auto path = m_current_track_row.get_path();
        auto iter = model->get_iter(path);
        
        while(iter) {
            iter++;
            if (!iter) break;
            
            Gtk::TreeModel::Row row = *iter;
            Glib::ustring name = row[m_columns.m_col_name];
            std::string ext = "";
            size_t dot_pos = name.find_last_of('.');
            if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                play_track(iter, model);
                return;
            }
        }
    }

    std::string format_time(gint64 seconds) {
        long mins = seconds / 60;
        long secs = seconds % 60;
        char buf[32];
        snprintf(buf, sizeof(buf), "%ld:%02ld", mins, secs);
        return std::string(buf);
    }

    void fetch_and_show_cover_art(const std::string& query) {
        int gen = m_selection_gen;
        std::thread([this, query, gen]() {
            std::string encoded_query = url_encode(query);
            std::string cmd = "curl -s \"https://itunes.apple.com/search?term=" + encoded_query + "&entity=song&limit=1\"";
            
            std::string result;
            std::array<char, 128> buffer;
            std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
            if (pipe) {
                while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                    result += buffer.data();
                }
            }

            std::regex re("\"artworkUrl100\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
            std::smatch match;
            if (std::regex_search(result, match, re) && match.size() > 1) {
                std::string artworkUrl = match.str(1);
                size_t pos = artworkUrl.find("100x100");
                if (pos != std::string::npos) artworkUrl.replace(pos, 7, "600x600");
                
                std::string temp_path = get_cache_dir() + "/temp_cover.jpg";
                std::string cmd_dl = "curl -s \"" + artworkUrl + "\" -o \"" + temp_path + "\"";
                if (system(cmd_dl.c_str()) == 0 && fs::exists(temp_path)) {
                    Glib::signal_idle().connect([this, temp_path, gen]() {
                        if (m_selection_gen == gen) display_image(temp_path);
                        return false;
                    });
                }
            }
        }).detach();
    }

    void update_metadata_from_tags(GstTagList* tags) {
        gchar *artist = nullptr, *title = nullptr, *album = nullptr, *genre = nullptr;
        GstDateTime *date = nullptr;
        std::string current_artist = m_entry_artist.get_text();
        std::string current_title = m_entry_title.get_text();

        if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &artist)) {
            m_entry_artist.set_text(artist);
            current_artist = artist;
            g_free(artist);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &title)) {
            m_entry_title.set_text(title);
            current_title = title;
            g_free(title);
        }

        if (!current_artist.empty() && !current_title.empty()) {
            set_title(current_artist + " - " + current_title);
        }

        if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &album)) {
            m_entry_album.set_text(album);
            g_free(album);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_GENRE, &genre)) {
            m_entry_genre.set_text(genre);
            g_free(genre);
        }
        if (gst_tag_list_get_date_time(tags, GST_TAG_DATE_TIME, &date)) {
            if (gst_date_time_has_year(date)) {
                m_entry_year.set_text(std::to_string(gst_date_time_get_year(date)));
            }
            gst_date_time_unref(date);
        }

        // Estrai immagine (copertina)
        GstSample *sample = nullptr;
        if (gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &sample)) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                try {
                    auto loader = Gdk::PixbufLoader::create();
                    loader->write(map.data, map.size);
                    loader->close();
                    auto pixbuf = loader->get_pixbuf();
                    // Ridimensiona se necessario (logica simile a display_image)
                    int width = m_col3_scrolled_window.get_allocation().get_width();
                    if (width <= 20) width = 300;
                    if (width > 20) {
                        int new_width = width - 20;
                        int new_height = (pixbuf->get_height() * new_width) / pixbuf->get_width();
                        pixbuf = pixbuf->scale_simple(new_width, new_height, Gdk::INTERP_BILINEAR);
                    }
                    m_image_preview.set(pixbuf);
                    m_image_preview.show();
                    m_automatic_cover_search_done = true;
                } catch(...) {}
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
        // Check also PREVIEW_IMAGE
        else if (gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &sample)) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                try {
                    auto loader = Gdk::PixbufLoader::create();
                    loader->write(map.data, map.size);
                    loader->close();
                    auto pixbuf = loader->get_pixbuf();
                    // Ridimensiona se necessario (logica simile a display_image)
                    int width = m_col3_scrolled_window.get_allocation().get_width();
                    if (width <= 20) width = 300;
                    if (width > 20) {
                        int new_width = width - 20;
                        int new_height = (pixbuf->get_height() * new_width) / pixbuf->get_width();
                        pixbuf = pixbuf->scale_simple(new_width, new_height, Gdk::INTERP_BILINEAR);
                    }
                    m_image_preview.set(pixbuf);
                    m_image_preview.show();
                    m_automatic_cover_search_done = true;
                } catch(...) {}
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }

        if (!m_automatic_cover_search_done && !m_image_preview.get_visible()) {
            std::string a = m_entry_artist.get_text();
            std::string t = m_entry_title.get_text();
            std::string query;

            if (!a.empty() && !t.empty()) {
                query = a + " " + t;
            } else {
                // Fallback: usa il nome del file dalla selezione corrente se i tag sono vuoti
                Glib::RefPtr<Gtk::TreeSelection> selection = m_db_view_active ? m_db_view.get_selection() : m_files_view.get_selection();
                auto iter = selection->get_selected();
                if (iter) {
                    Gtk::TreeModel::Row row = *iter;
                    std::string name = (Glib::ustring)row[m_columns.m_col_name];
                    size_t dot_pos = name.find_last_of('.');
                    if (dot_pos != std::string::npos) name = name.substr(0, dot_pos);
                    query = name;
                }
            }

            if (!query.empty()) {
                m_automatic_cover_search_done = true;
                fetch_and_show_cover_art(query);
            }
        }
    }

    void fill_metadata_from_tags(GstTagList* tags, Metadata& meta) {
        gchar *artist = nullptr, *title = nullptr, *album = nullptr, *genre = nullptr;
        GstDateTime *date = nullptr;

        if (gst_tag_list_get_string(tags, GST_TAG_ARTIST, &artist)) {
            meta.artist = artist ? artist : "";
            g_free(artist);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &title)) {
            meta.title = title ? title : "";
            g_free(title);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_ALBUM, &album)) {
            meta.album = album ? album : "";
            g_free(album);
        }
        if (gst_tag_list_get_string(tags, GST_TAG_GENRE, &genre)) {
            meta.genre = genre ? genre : "";
            g_free(genre);
        }
        if (gst_tag_list_get_date_time(tags, GST_TAG_DATE_TIME, &date)) {
            if (gst_date_time_has_year(date)) {
                meta.year = std::to_string(gst_date_time_get_year(date));
            }
            gst_date_time_unref(date);
        }

        guint bitrate = 0;
        if (gst_tag_list_get_uint(tags, GST_TAG_BITRATE, &bitrate)) {
            meta.bitrate = bitrate;
        } else if (gst_tag_list_get_uint(tags, GST_TAG_NOMINAL_BITRATE, &bitrate)) {
            meta.bitrate = bitrate;
        }
        gchar *codec = nullptr;
        if (gst_tag_list_get_string(tags, GST_TAG_AUDIO_CODEC, &codec)) {
            meta.codec = codec ? codec : "";
            g_free(codec);
        }

        // Estrai copertina in memoria
        GstSample *sample = nullptr;
        if (gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &sample) || 
            gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &sample)) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                meta.rawCoverData.assign((char*)map.data, (char*)map.data + map.size);
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
    }

    Metadata extract_metadata_from_memory(const std::vector<char>& data) {
        Metadata meta;
        meta.size_mb = (double)data.size() / (1024.0 * 1024.0);

        GstElement *pipeline = gst_pipeline_new("mem_meta_pipe");
        GstElement *src = gst_element_factory_make("giostreamsrc", "src");
        GstElement *decodebin = gst_element_factory_make("decodebin", "decode");
        GstElement *fakesink = gst_element_factory_make("fakesink", "sink");

        if (!pipeline || !src || !decodebin || !fakesink) {
            if (pipeline) gst_object_unref(pipeline);
            return meta;
        }

        // Setup memory stream source
        GInputStream *stream = g_memory_input_stream_new_from_data(data.data(), data.size(), NULL);
        g_object_set(src, "stream", stream, NULL);
        g_object_unref(stream); // L'elemento prende ownership o ref

        gst_bin_add_many(GST_BIN(pipeline), src, decodebin, fakesink, NULL);
        gst_element_link(src, decodebin);

        struct CallbackData {
            GstElement* sink;
            GstPad** audio_pad;
        };
        GstPad* audio_pad = nullptr;
        CallbackData cb_data = { fakesink, &audio_pad };

        // Logica identica a extract_metadata_internal per i segnali
        g_signal_connect(decodebin, "pad-added", G_CALLBACK(+[](GstElement* /*element*/, GstPad* pad, gpointer user_data){
            CallbackData* data = (CallbackData*)user_data;
            GstCaps *caps = gst_pad_query_caps(pad, NULL);
            bool is_audio = false;
            if (caps) {
                for(guint i = 0; i < gst_caps_get_size(caps); ++i) {
                    const GstStructure *str = gst_caps_get_structure(caps, i);
                    if (g_str_has_prefix(gst_structure_get_name(str), "audio/")) {
                        is_audio = true;
                        break;
                    }
                }
                gst_caps_unref(caps);
            }
            if (is_audio) {
                GstPad *sink_pad = gst_element_get_static_pad(data->sink, "sink");
                if (!gst_pad_is_linked(sink_pad)) {
                    if (gst_pad_link(pad, sink_pad) == GST_PAD_LINK_OK) {
                        if (*(data->audio_pad) == nullptr) *(data->audio_pad) = (GstPad*)gst_object_ref(pad);
                    }
                }
                gst_object_unref(sink_pad);
            }
        }), &cb_data);

        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        run_metadata_loop(pipeline, meta, audio_pad);
        
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return meta;
    }

    Metadata extract_metadata_internal(const std::string& local_path) {
        Metadata meta;
        // Get file size
        try { meta.size_mb = (double)fs::file_size(local_path) / (1024.0 * 1024.0); } catch(...) {}

        GstElement *pipeline = gst_pipeline_new("meta_pipe");
        GstElement *src = gst_element_factory_make("filesrc", "src");
        GstElement *decodebin = gst_element_factory_make("decodebin", "decode");
        GstElement *fakesink = gst_element_factory_make("fakesink", "sink");

        if (!pipeline || !src || !decodebin || !fakesink) {
            if (pipeline) gst_object_unref(pipeline);
            return meta;
        }
        
        g_object_set(src, "location", local_path.c_str(), NULL);
        gst_bin_add_many(GST_BIN(pipeline), src, decodebin, fakesink, NULL);
        gst_element_link(src, decodebin);

        struct CallbackData {
            GstElement* sink;
            GstPad** audio_pad;
        };
        
        GstPad* audio_pad = nullptr;
        CallbackData cb_data = { fakesink, &audio_pad };

        g_signal_connect(decodebin, "pad-added", G_CALLBACK(+[](GstElement* /*element*/, GstPad* pad, gpointer user_data){
            CallbackData* data = (CallbackData*)user_data;
            // Usa query_caps per verificare se è audio anche se non ancora negoziato
            GstCaps *caps = gst_pad_query_caps(pad, NULL);
            bool is_audio = false;
            if (caps) {
                for(guint i = 0; i < gst_caps_get_size(caps); ++i) {
                    const GstStructure *str = gst_caps_get_structure(caps, i);
                    if (g_str_has_prefix(gst_structure_get_name(str), "audio/")) {
                        is_audio = true;
                        break;
                    }
                }
                gst_caps_unref(caps);
            }

            if (is_audio) {
                GstPad *sink_pad = gst_element_get_static_pad(data->sink, "sink");
                if (!gst_pad_is_linked(sink_pad)) {
                    if (gst_pad_link(pad, sink_pad) == GST_PAD_LINK_OK) {
                        // Salviamo il pad per interrogarlo dopo il preroll
                        if (*(data->audio_pad) == nullptr) {
                            *(data->audio_pad) = (GstPad*)gst_object_ref(pad);
                        }
                    }
                }
                gst_object_unref(sink_pad);
            }
        }), &cb_data);
        
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        
        run_metadata_loop(pipeline, meta, audio_pad);

        // Mantieni compatibilità file-based: se abbiamo estratto una cover in RAM, salviamola su disco
        if (!meta.rawCoverData.empty()) {
            std::string cover_fn = local_path + ".cover.jpg";
            std::ofstream f(cover_fn, std::ios::binary);
            f.write(meta.rawCoverData.data(), meta.rawCoverData.size());
            f.close();
            meta.coverPath = cover_fn;
        }

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return meta;
    }

    void run_metadata_loop(GstElement* pipeline, Metadata& meta, GstPad*& audio_pad) {
        GstBus *bus = gst_element_get_bus(pipeline);
        bool done = false;
        gint64 timeout_ns = 2000 * GST_MSECOND;
        
        GstTagList *final_tags = nullptr;

        while (!done) {
            GstMessage *msg = gst_bus_timed_pop_filtered(bus, 100 * GST_MSECOND, 
                (GstMessageType)(GST_MESSAGE_TAG | GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            
            if (msg) {
                switch (GST_MESSAGE_TYPE(msg)) {
                    case GST_MESSAGE_TAG: {
                        GstTagList *tags = nullptr;
                        gst_message_parse_tag(msg, &tags);
                        if (!final_tags) {
                            final_tags = tags;
                        } else {
                            gst_tag_list_insert(final_tags, tags, GST_TAG_MERGE_REPLACE);
                            gst_tag_list_unref(tags);
                        }
                        break;
                    }
                    case GST_MESSAGE_ASYNC_DONE:
                    case GST_MESSAGE_ERROR:
                    case GST_MESSAGE_EOS:
                        done = true;
                        break;
                    default: break;
                }
                gst_message_unref(msg);
            }
            timeout_ns -= 100 * GST_MSECOND;
            if (timeout_ns <= 0) done = true;
        }
        
        if (final_tags) {
            fill_metadata_from_tags(final_tags, meta);
            gst_tag_list_unref(final_tags);
        }
        
        // Estrai Sample Rate dal pad audio negoziato
        if (audio_pad) {
            GstCaps *caps = gst_pad_get_current_caps(audio_pad);
            if (caps) {
                const GstStructure *str = gst_caps_get_structure(caps, 0);
                gst_structure_get_int(str, "rate", (gint*)&meta.samplerate);
                gst_caps_unref(caps);
            }
            gst_object_unref(audio_pad);
        }

        // Fallback Bitrate calculation
        if (meta.bitrate == 0) {
            gint64 dur = 0;
            if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &dur) && dur > 0) {
                uint64_t size_bytes = (uint64_t)(meta.size_mb * 1024.0 * 1024.0);
                double seconds = (double)dur / GST_SECOND;
                meta.bitrate = (unsigned int)((size_bytes * 8) / seconds);
            }
        }
        gst_object_unref(bus);
    }

    void on_search_online_clicked() {
        std::string artist = m_entry_artist.get_text();
        std::string title = m_entry_title.get_text();
        std::string query;

        // Se i campi sono vuoti, prova a usare il nome del file
        if (artist.empty() && title.empty()) {
            auto selection = m_files_view.get_selection();
            auto iter = selection->get_selected();
            if (iter) {
                Gtk::TreeModel::Row row = *iter;
                Glib::ustring filename = row[m_columns.m_col_name];
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) filename = filename.substr(0, dot_pos);
                query = filename;
            }
        } else {
            query = artist + " " + title;
        }

        if (query.empty()) return;

        if (query == m_last_query) {
            m_search_index++;
        } else {
            m_search_index = 0;
            m_last_query = query;
        }
        int target_index = m_search_index;

        // Esegui ricerca in background
        std::thread([this, query, target_index]() {
            std::string encoded_query = url_encode(query);
            // Usa iTunes Search API con limit 20 per poter ciclare
            std::string cmd = "curl -s \"https://itunes.apple.com/search?term=" + encoded_query + "&entity=song&limit=20\"";
            
            std::array<char, 128> buffer;
            std::string result;
            std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
            if (pipe) {
                while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                    result += buffer.data();
                }
            }

            // Parsing JSON manuale per isolare gli oggetti
            std::vector<std::string> objects;
            size_t results_pos = result.find("\"results\"");
            if (results_pos != std::string::npos) {
                size_t array_start = result.find('[', results_pos);
                if (array_start != std::string::npos) {
                    size_t current_pos = array_start + 1;
                    while (current_pos < result.length()) {
                        size_t obj_start = result.find('{', current_pos);
                        if (obj_start == std::string::npos) break;
                        
                        int balance = 1;
                        size_t obj_end = obj_start + 1;
                        bool in_quote = false;
                        while (obj_end < result.length() && balance > 0) {
                            char c = result[obj_end];
                            if (c == '"' && (obj_end == 0 || result[obj_end-1] != '\\')) {
                                in_quote = !in_quote;
                            } else if (!in_quote) {
                                if (c == '{') balance++;
                                else if (c == '}') balance--;
                            }
                            obj_end++;
                        }
                        
                        if (balance == 0) {
                            objects.push_back(result.substr(obj_start, obj_end - obj_start));
                            current_pos = obj_end;
                        } else {
                            break;
                        }
                    }
                }
            }

            Metadata meta;
            std::string temp_art_path;
            bool found = false;

            if (!objects.empty()) {
                int actual_index = target_index % objects.size();
                std::string object_json = objects[actual_index];
                found = true;

                auto get_json_val = [&](const std::string& key) -> std::string {
                    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
                    std::smatch match;
                    if (std::regex_search(object_json, match, re) && match.size() > 1) return match.str(1);
                    return "";
                };

                meta.artist = get_json_val("artistName");
                meta.title = get_json_val("trackName");
                meta.album = get_json_val("collectionName");
                meta.genre = get_json_val("primaryGenreName");
                meta.artworkUrl = get_json_val("artworkUrl100");
                
                std::string date = get_json_val("releaseDate");
                if (date.size() >= 4) meta.year = date.substr(0, 4);

                if (!meta.artworkUrl.empty()) {
                    size_t pos = meta.artworkUrl.find("100x100");
                    if (pos != std::string::npos) {
                        meta.artworkUrl.replace(pos, 7, "600x600");
                    }
                    temp_art_path = "/tmp/temp_cover.jpg";
                    temp_art_path = get_cache_dir() + "/temp_cover.jpg";
                    std::string cmd_dl = "curl -s \"" + meta.artworkUrl + "\" -o \"" + temp_art_path + "\"";
                    system(cmd_dl.c_str());
                }
            }

            // Aggiorna UI
            Glib::signal_idle().connect([this, meta, temp_art_path, found]() {
                if (found) {
                    if (!meta.artist.empty()) m_entry_artist.set_text(meta.artist);
                    if (!meta.title.empty()) m_entry_title.set_text(meta.title);
                    if (!meta.album.empty()) m_entry_album.set_text(meta.album);
                    if (!meta.year.empty()) m_entry_year.set_text(meta.year);
                    if (!meta.genre.empty()) m_entry_genre.set_text(meta.genre);
                    
                    if (!temp_art_path.empty() && fs::exists(temp_art_path)) {
                        display_image(temp_art_path);
                    }
                } else {
                     Gtk::MessageDialog dlg(*this, "Nessun risultato trovato.", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                     dlg.run();
                }
                return false;
            });
        }).detach();
    }

    void on_recognize_clicked() {
        if (m_db_view_active) {
            auto selection = m_db_view.get_selection();
            std::vector<Gtk::TreeModel::Path> rows = selection->get_selected_rows();
            
            if (rows.size() > 1) {
                // --- MODALITÀ BATCH (Selezione multipla) ---
                std::vector<std::string> batch_paths;
                for (auto path : rows) {
                    auto iter = m_db_model->get_iter(path);
                    if (iter) {
                        batch_paths.push_back((Glib::ustring)(*iter)[m_columns.m_col_path]);
                    }
                }

                m_btn_recognize.set_sensitive(false);
                m_btn_recognize.set_label("Avvio Batch...");

                std::thread([this, batch_paths]() {
                    int total = batch_paths.size();
                    int processed = 0;
                    int updated = 0;
                    
                    // Helper per trovare lo script (copiato dalla logica singola)
                    char self_path_buf[PATH_MAX] = {0};
                    ssize_t count = readlink("/proc/self/exe", self_path_buf, PATH_MAX);
                    std::string exe_path = (count > 0) ? std::string(self_path_buf, count) : "";
                    std::string script_path = fs::path(exe_path).parent_path().string() + "/google_recognize.sh";

                    if (!fs::exists(script_path)) {
                         Glib::signal_idle().connect([this, script_path]() {
                             Gtk::MessageDialog dlg(*this, "Script non trovato: " + script_path, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                             dlg.run();
                             m_btn_recognize.set_sensitive(true);
                             m_btn_recognize.set_label("Invia a SongRep");
                             return false;
                         });
                         return;
                    }

                    for (const auto& smb_path : batch_paths) {
                        processed++;
                        Glib::signal_idle().connect([this, processed, total]() {
                            m_btn_recognize.set_label("Batch: " + std::to_string(processed) + "/" + std::to_string(total));
                            return false;
                        });

                        std::string ext = fs::path(smb_path).extension();
                        std::string ext_lower = ext;
                        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                        
                        if (ext_lower != ".mp3" && ext_lower != ".m4a" && ext_lower != ".flac" && ext_lower != ".wav" && ext_lower != ".ogg") continue;

                        std::string local_path = "/tmp/batch_rec_" + std::to_string(std::rand()) + ext_lower;
                        
                        if (!download_smb_file(smb_path, local_path, m_config)) continue;

                        std::string cmd = "bash \"" + script_path + "\" \"" + local_path + "\" 2>&1";
                        std::string rec_output;
                        std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
                        if (pipe) {
                            std::array<char, 256> buffer;
                            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                                std::string line = buffer.data();
                                if (line.find("STATUS:") == std::string::npos) {
                                    rec_output += line;
                                }
                            }
                        }
                        fs::remove(local_path);

                        // Parsing Risultato "Titolo by Artista"
                        if (!rec_output.empty() && rec_output.back() == '\n') rec_output.pop_back();
                        size_t by_pos = rec_output.find(" by ");
                        if (by_pos != std::string::npos) {
                            std::string title = rec_output.substr(0, by_pos);
                            std::string artist = rec_output.substr(by_pos + 4);
                            
                            if (title != "Sconosciuto" && !title.empty()) {
                                // Ricerca Metadati Online (iTunes API)
                                std::string query = artist + " " + title;
                                std::string encoded_query = url_encode(query);
                                std::string search_cmd = "curl -s \"https://itunes.apple.com/search?term=" + encoded_query + "&entity=song&limit=1\"";
                                
                                std::string search_json;
                                std::unique_ptr<FILE, int(*)(FILE*)> spipe(popen(search_cmd.c_str(), "r"), pclose);
                                if (spipe) {
                                    std::array<char, 128> buf;
                                    while (fgets(buf.data(), buf.size(), spipe.get()) != nullptr) search_json += buf.data();
                                }

                                // Estrazione JSON minimale
                                auto get_json_val = [&](const std::string& json, const std::string& key) -> std::string {
                                    std::regex re("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
                                    std::smatch match;
                                    if (std::regex_search(json, match, re) && match.size() > 1) return match.str(1);
                                    return "";
                                };

                                std::string found_artist = get_json_val(search_json, "artistName");
                                std::string found_title = get_json_val(search_json, "trackName");
                                std::string found_album = get_json_val(search_json, "collectionName");
                                std::string found_genre = get_json_val(search_json, "primaryGenreName");
                                std::string artworkUrl = get_json_val(search_json, "artworkUrl100");
                                std::string date = get_json_val(search_json, "releaseDate");
                                std::string found_year = (date.size() >= 4) ? date.substr(0, 4) : "";

                                if (found_artist.empty()) found_artist = artist;
                                if (found_title.empty()) found_title = title;

                                std::string cover_path_db = "";
                                if (!artworkUrl.empty()) {
                                    size_t pos = artworkUrl.find("100x100");
                                    if (pos != std::string::npos) artworkUrl.replace(pos, 7, "600x600");
                                    
                                    std::string cache_path = get_hashed_path(smb_path, ".jpg");
                                    std::string cmd_dl = "curl -s \"" + artworkUrl + "\" -o \"" + cache_path + "\"";
                                    if (system(cmd_dl.c_str()) == 0) cover_path_db = cache_path;
                                }

                                // Aggiornamento DB
                                if (m_db) {
                                    std::string sql = "UPDATE music SET artist=?, title=?, album=?, year=?, genre=?, coverPath=? WHERE path=?;";
                                    sqlite3_stmt* stmt;
                                    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                                        sqlite3_bind_text(stmt, 1, found_artist.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 2, found_title.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 3, found_album.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 4, found_year.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 5, found_genre.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 6, cover_path_db.c_str(), -1, SQLITE_TRANSIENT);
                                        sqlite3_bind_text(stmt, 7, smb_path.c_str(), -1, SQLITE_TRANSIENT);
                                        if (sqlite3_step(stmt) == SQLITE_DONE) updated++;
                                        sqlite3_finalize(stmt);
                                    }
                                }
                            }
                        }
                    }
                    
                    Glib::signal_idle().connect([this, updated]() {
                        m_btn_recognize.set_sensitive(true);
                        m_btn_recognize.set_label("Invia a SongRep");
                        if (m_db_view_active) load_db_view();
                        Gtk::MessageDialog dlg(*this, "Batch completato. Aggiornati " + std::to_string(updated) + " brani.", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                        dlg.run();
                        return false;
                    });
                }).detach();
                return;
            }
        }

        Glib::RefPtr<Gtk::TreeSelection> selection;
        if (m_db_view_active) {
            selection = m_db_view.get_selection();
        } else {
            selection = m_files_view.get_selection();
        }

        auto iter = selection->get_selected();
        if (!iter) return;

        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring smb_path = row[m_columns.m_col_path];

        std::string ext = "";
        // Usa smb_path per l'estensione perché 'name' nella vista DB potrebbe essere "Artista - Titolo"
        std::string path_str = smb_path;
        size_t dot_pos = path_str.find_last_of('.');
        if (dot_pos != std::string::npos) ext = path_str.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext != ".mp3" && ext != ".wav" && ext != ".flac" && ext != ".ogg" && ext != ".m4a") {
             Gtk::MessageDialog dlg(*this, "Formato non supportato per il riconoscimento.", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK);
             dlg.run();
             return;
        }

        m_btn_recognize.set_sensitive(false);
        m_btn_recognize.set_label("Riconoscimento..."); // Give user feedback
    
        std::thread([this, smb_path, ext]() {
            std::string local_path = "/tmp/rec_temp" + ext;
            if (fs::exists(local_path)) fs::remove(local_path);

            if (download_smb_file(smb_path, local_path, m_config)) {
                // Usa lo script google_recognize.sh fornito
                char self_path_buf[PATH_MAX] = {0};
                ssize_t count = readlink("/proc/self/exe", self_path_buf, PATH_MAX);
                std::string exe_path = (count > 0) ? std::string(self_path_buf, count) : "";
                std::string script_path = fs::path(exe_path).parent_path().string() + "/google_recognize.sh";
                
                // Verifica esistenza script
                if (!fs::exists(script_path)) {
                    Glib::signal_idle().connect([this, script_path]() {
                        m_btn_recognize.set_sensitive(true);
                        m_btn_recognize.set_label("Invia a SongRep");
                        Gtk::MessageDialog dlg(*this, "Errore File", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                        dlg.set_secondary_text("Script non trovato: " + script_path);
                        dlg.run();
                        return false;
                    });
                    return;
                }

                std::string cmd = "bash \"" + script_path + "\" \"" + local_path + "\" 2>&1";
                
                std::array<char, 256> buffer; // Larger buffer
                std::string result;
                std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
                
                if (pipe) {
                    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                        std::string line = buffer.data();
                        // Gestione Feedback Real-time
                        if (line.find("STATUS: ") == 0) {
                            std::string status_msg = line.substr(8);
                            if (!status_msg.empty() && status_msg.back() == '\n') status_msg.pop_back();
                            
                            Glib::signal_idle().connect([this, status_msg]() {
                                m_btn_recognize.set_label(status_msg);
                                return false;
                            });
                            continue; // Non aggiungere status al risultato finale
                        }
                        result += line;
                    }
                }

                Glib::signal_idle().connect([this, result, local_path, smb_path]() mutable {
                    m_btn_recognize.set_sensitive(true);
                    m_btn_recognize.set_label("Invia a SongRep"); // Restore label
                    if (fs::exists(local_path)) fs::remove(local_path);

                    // Verifica che la selezione non sia cambiata nel frattempo
                    Glib::RefPtr<Gtk::TreeSelection> selection;
                    if (m_db_view_active) {
                        selection = m_db_view.get_selection();
                    } else {
                        selection = m_files_view.get_selection();
                    }
                    
                    auto iter = selection->get_selected();
                    if (!iter || (Glib::ustring)(*iter)[m_columns.m_col_path] != smb_path) {
                        return false;
                    }

                    // Pulisci output da eventuali newline finali
                    if (!result.empty() && result.back() == '\n') {
                        result.pop_back();
                    }

                    // Lo script restituisce "Titolo by Artista" o un messaggio di errore
                    std::string title, artist;
                    size_t by_pos = result.find(" by ");
                    if (by_pos != std::string::npos) {
                        title = result.substr(0, by_pos);
                        artist = result.substr(by_pos + 4);
                    }

                    if (!title.empty() && title != "Sconosciuto") {
                        m_entry_title.set_text(title);
                        m_entry_artist.set_text(artist);
                        
                        // Chiedi all'utente se vuole cercare online anche gli altri metadati
                        Gtk::MessageDialog dlg(*this, "Riconoscimento completato", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
                        dlg.set_secondary_text("Vuoi cercare online i metadati completi (album, anno, copertina) per '" + title + "'?");
                        if (dlg.run() == Gtk::RESPONSE_YES) {
                            on_search_online_clicked();
                        }
                    } else {
                        Gtk::MessageDialog dlg(*this, "Riconoscimento fallito", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                        if (result.empty()) result = "Nessun output ricevuto dallo script.";
                        dlg.set_secondary_text("Dettagli:\n" + result);
                        dlg.run();
                    }
                    return false;
                });
            } else {
                Glib::signal_idle().connect([this]() {
                    m_btn_recognize.set_sensitive(true);
                    m_btn_recognize.set_label("Invia a SongRep");
                    Gtk::MessageDialog dlg(*this, "Errore nel download del file per il riconoscimento.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                    dlg.run();
                    return false;
                });
            }
        }).detach();
    }

    void on_save_metadata_clicked() {
        perform_save_metadata(false);
    }

    void on_save_db_only_clicked() {
        if (!m_db_view_active) return;
        
        auto selection = m_db_view.get_selection();
        auto iter = selection->get_selected();
        if (!iter) return;

        // Salva riferimento alla riga perché l'ordinamento potrebbe spostarla
        Gtk::TreeRowReference row_ref(m_db_model, m_db_model->get_path(iter));

        Gtk::TreeModel::Row row = *iter;
        std::string path = (Glib::ustring)row[m_columns.m_col_path];

        std::string artist = m_entry_artist.get_text();
        std::string title = m_entry_title.get_text();
        std::string album = m_entry_album.get_text();
        std::string year = m_entry_year.get_text();
        std::string genre = m_entry_genre.get_text();

        if (m_db) {
            const char* sql = "UPDATE music SET artist=?, title=?, album=?, year=?, genre=? WHERE path=?;";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0) == SQLITE_OK) {
                 sqlite3_bind_text(stmt, 1, artist.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(stmt, 3, album.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(stmt, 4, year.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(stmt, 5, genre.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(stmt, 6, path.c_str(), -1, SQLITE_TRANSIENT);
                 
                 if (sqlite3_step(stmt) == SQLITE_DONE) {
                     // Aggiorna Modello UI
                     row[m_columns.m_col_artist] = artist;
                     row[m_columns.m_col_title] = title;
                     row[m_columns.m_col_album] = album;
                     row[m_columns.m_col_year] = year;
                     row[m_columns.m_col_genre] = genre;
                     
                     if (!artist.empty() && !title.empty()) {
                        row[m_columns.m_col_name] = artist + " - " + title;
                     }

                     // Ripristina focus e scroll sulla riga (che potrebbe essersi spostata)
                     if (row_ref.is_valid()) {
                         auto new_path = row_ref.get_path();
                         m_db_view.scroll_to_row(new_path);
                         m_db_view.get_selection()->select(new_path);
                     }
                     
                     update_db_labels(path);

                     Gtk::MessageDialog dlg(*this, "Database aggiornato.", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                     dlg.run();
                 } else {
                     Gtk::MessageDialog dlg(*this, "Errore aggiornamento DB.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                     dlg.run();
                 }
                 sqlite3_finalize(stmt);
            }
        }
    }

    void perform_save_metadata(bool silent) {
        LOG(INFO, "Save metadata clicked");
        Glib::RefPtr<Gtk::TreeSelection> selection;
        if (m_db_view_active) {
            selection = m_db_view.get_selection();
        } else {
            selection = m_files_view.get_selection();
        }

        auto iter = selection->get_selected();
        if (!iter) return;

        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring smb_path = row[m_columns.m_col_path];

        std::string ext = "";
        // Usa smb_path per l'estensione (il nome visualizzato nel DB potrebbe non averla)
        std::string path_str = (std::string)smb_path;
        size_t dot_pos = path_str.find_last_of('.');
        if (dot_pos != std::string::npos) ext = path_str.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Supporto scrittura solo per MP3 per ora
        if (ext != ".mp3") {
            if (!silent) {
                Gtk::MessageDialog dlg(*this, "Modifica supportata solo per file MP3", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                dlg.run();
            }
            return;
        }

        // Usa percorsi temporanei univoci per evitare conflitti con la riproduzione
        std::string work_path = "/tmp/edit_work_" + std::to_string(std::rand()) + ext;
        std::string output_path = "/tmp/edit_out_" + std::to_string(std::rand()) + ext;

        // 1. Scarica il file specifico da modificare (fondamentale!)
        if (!download_smb_file(smb_path, work_path, m_config)) {
             if (!silent) {
                 Gtk::MessageDialog dlg(*this, "Errore: impossibile scaricare il file per la modifica.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                 dlg.run();
             }
             return;
        }

        // Pipeline per scrivere i tag: filesrc -> mpegaudioparse -> id3mux -> filesink
        GstElement *pipeline = gst_pipeline_new("tag-writer");
        GstElement *src = gst_element_factory_make("filesrc", "src");
        GstElement *parse = gst_element_factory_make("mpegaudioparse", "parse");
        GstElement *mux = gst_element_factory_make("id3mux", "mux");
        GstElement *sink = gst_element_factory_make("filesink", "sink");

        if (!pipeline || !src || !parse || !mux || !sink) {
            if (pipeline) gst_object_unref(pipeline);
            if (fs::exists(work_path)) fs::remove(work_path);
            return;
        }

        g_object_set(src, "location", work_path.c_str(), NULL);
        g_object_set(sink, "location", output_path.c_str(), NULL);

        gst_bin_add_many(GST_BIN(pipeline), src, parse, mux, sink, NULL);
        gst_element_link_many(src, parse, mux, sink, NULL);

        // Imposta i tag
        GstTagList *tags = gst_tag_list_new_empty();
        gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_ARTIST, m_entry_artist.get_text().c_str(), NULL);
        gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_TITLE, m_entry_title.get_text().c_str(), NULL);
        gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_ALBUM, m_entry_album.get_text().c_str(), NULL);
        gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_GENRE, m_entry_genre.get_text().c_str(), NULL);
        
        std::string year_str = m_entry_year.get_text();
        if (!year_str.empty()) {
            try {
                GstDateTime *dt = gst_date_time_new_y(std::stoi(year_str));
                gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_DATE_TIME, dt, NULL);
                gst_date_time_unref(dt);
            } catch (...) {}
        }

        // Aggiungi copertina se presente
        std::string cover_path = "/tmp/temp_cover.jpg";
        cover_path = get_cache_dir() + "/temp_cover.jpg";
        std::string new_cache_path;

        if (fs::exists(cover_path)) {
            std::ifstream file(cover_path, std::ios::binary | std::ios::ate);
            if (file) {
                std::streamsize size = file.tellg();
                file.seekg(0, std::ios::beg);
                
                if (size > 0) {
                    GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
                    GstMapInfo map;
                    if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
                        file.read((char*)map.data, size);
                        gst_buffer_unmap(buf, &map);
                        
                        GstCaps *caps = gst_caps_new_empty_simple("image/jpeg");
                        GstSample *sample = gst_sample_new(buf, caps, NULL, NULL);
                        
                        gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE, GST_TAG_IMAGE, sample, NULL);
                        
                        gst_sample_unref(sample);
                        gst_caps_unref(caps);
                        
                        new_cache_path = get_hashed_path(smb_path, ".jpg");
                    }
                    gst_buffer_unref(buf);
                }
            }
        }

        gst_tag_setter_merge_tags(GST_TAG_SETTER(mux), tags, GST_TAG_MERGE_REPLACE);
        gst_tag_list_unref(tags);

        // Esegui pipeline
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        
        bool success = false;
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                success = true;
            } else {
                GError *err = NULL;
                gchar *debug = NULL;
                gst_message_parse_error(msg, &err, &debug);
                LOG(ERROR, "GStreamer Tagging Error: " << (err ? err->message : "unknown"));
                if (err) g_error_free(err);
                if (debug) g_free(debug);
            }
            gst_message_unref(msg);
        }
        
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        if (success && fs::exists(output_path)) {
            // Usa il nome file reale dal percorso, non il nome visualizzato nella UI
            std::string remote_filename = fs::path((std::string)smb_path).filename().string();
            std::string cmd = "put \"" + output_path + "\" \"" + remote_filename + "\"";
            
            // Estrai la cartella genitore per smbclient (non passare il percorso completo del file!)
            std::string parent_dir = smb_path;
            size_t last_slash = parent_dir.find_last_of('/');
            if (last_slash != std::string::npos) {
                parent_dir = parent_dir.substr(0, last_slash);
            }

            auto upload_res = exec_smb_command(parent_dir, m_config, cmd);
            if (!upload_res.first || upload_res.second.find("NT_STATUS_") != std::string::npos) {
                if (!silent) {
                    Gtk::MessageDialog dlg(*this, "Errore caricamento SMB: " + upload_res.second, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                    dlg.run();
                }
            } else {
                // Aggiorna cache
                Metadata meta;
                meta.artist = m_entry_artist.get_text();
                meta.title = m_entry_title.get_text();
                meta.album = m_entry_album.get_text();
                meta.year = m_entry_year.get_text();
                meta.genre = m_entry_genre.get_text();
                // Preserve tech info
                if (m_meta_cache.count((std::string)smb_path)) {
                    auto& old = m_meta_cache[(std::string)smb_path];
                    meta.bitrate = old.bitrate; meta.codec = old.codec; meta.samplerate = old.samplerate; meta.size_mb = old.size_mb;
                }

                // Preserva coverPath esistente se c'era
                if (!new_cache_path.empty()) {
                    fs::copy_file(cover_path, new_cache_path, fs::copy_options::overwrite_existing);
                    meta.coverPath = new_cache_path;
                } else if (m_meta_cache.count((std::string)smb_path)) {
                    meta.coverPath = m_meta_cache[(std::string)smb_path].coverPath;
                }
                m_meta_cache[(std::string)smb_path] = meta;

                if (!silent) {
                    Gtk::MessageDialog dlg(*this, "Metadati salvati con successo!", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                    dlg.run();
                }
            }
        } else {
            if (!silent) {
                Gtk::MessageDialog dlg(*this, "Errore durante la scrittura dei tag (Pipeline failed).", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                dlg.run();
            }
        }

        // Pulizia file temporanei
        if (fs::exists(work_path)) fs::remove(work_path);
        if (fs::exists(output_path)) fs::remove(output_path);
    }

    bool parse_smb_line(const std::string& line, std::string& name, bool& is_folder, bool& is_hidden) {
        static const std::regex re(R"(^\s+(.+?)\s{2,}([DAHSR]*)\s+\d+\s+)");
        std::smatch match;
        if (std::regex_search(line, match, re) && match.size() > 2) {
            name = match.str(1);
            std::string attrs = match.str(2);
            name.erase(name.find_last_not_of(' ') + 1);
            if (name == "." || name == "..") return false;
             is_hidden = (attrs.find('H') != std::string::npos);
            is_folder = (attrs.find('D') != std::string::npos);
            return true;
        }
        return false;
    }

    void on_scale_value_changed() {
        if (!m_updating_scale && m_pipeline) {
            gint64 value = static_cast<gint64>(m_scale.get_value());
            gst_element_seek_simple(m_pipeline, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), value * GST_SECOND);
        }
    }

    void on_play_clicked() {
        LOG(DEBUG, "on_play_clicked called.");
        LOG(INFO, "Play button clicked");
        auto selection = m_files_view.get_selection();
        auto iter = selection->get_selected();
        if (iter) {
            bool is_same_track = false;
            if (m_current_track_row.is_valid()) {
                auto current_path = m_current_track_row.get_path();
                auto selected_path = m_files_model->get_path(iter);
                if (current_path == selected_path) {
                    is_same_track = true;
                }
            }

            if (is_same_track && m_pipeline) {
                GstState current_state;
                gst_element_get_state(m_pipeline, &current_state, NULL, 0);
                if (current_state == GST_STATE_PAUSED || current_state == GST_STATE_READY) {
                    LOG(DEBUG, "Resuming playback of current track.");
                    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                } else {
                    LOG(DEBUG, "Current track already playing or in an unexpected state. Re-playing selected track.");
                    play_track(iter, m_files_model);
                }
            } else {
                LOG(DEBUG, "New track selected or no track playing. Starting playback of selected track.");
                play_track(iter, m_files_model);
            }
        } else if (m_pipeline) {
            GstState current_state;
            gst_element_get_state(m_pipeline, &current_state, NULL, 0);
            if (current_state == GST_STATE_PAUSED || current_state == GST_STATE_READY) {
                LOG(DEBUG, "No track selected, but pipeline exists and is paused/ready. Resuming playback.");
                gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
            } else {
                LOG(INFO, "Play clicked but no track selected and pipeline not in paused/ready state.");
            }
        } else {
            LOG(INFO, "Play clicked but no track selected and no pipeline initialized.");
        }
    }

    void on_stop_clicked() {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            m_updating_scale = true;
            m_scale.set_value(0);
            m_updating_scale = false;
            set_title("Music Network Player " + APP_VERSION);
            LOG(DEBUG, "Playback stopped and pipeline set to NULL.");
        }
    }
    void on_pause_clicked() {
        if (m_pipeline) { gst_element_set_state(m_pipeline, GST_STATE_PAUSED); LOG(DEBUG, "Playback paused."); }
    }

    bool on_folder_button_press(GdkEventButton* event) {
        if (event->type == GDK_BUTTON_PRESS && event->button == 3) { // Tasto destro
            auto selection = m_folder_view.get_selection();
            if (selection->count_selected_rows() > 0) {
                m_context_view = &m_folder_view;
                m_context_menu.popup_at_pointer((GdkEvent*)event);
                return true;
            }
        }
        return false;
    }

    bool on_files_button_press(GdkEventButton* event) {
        if (event->type == GDK_BUTTON_PRESS && event->button == 3) { // Tasto destro
            auto selection = m_files_view.get_selection();
            if (selection->count_selected_rows() > 0) {
                m_context_view = &m_files_view;
                m_context_menu.popup_at_pointer((GdkEvent*)event);
                return true;
            }
        }
        return false;
    }

    bool on_ipod_button_press(GdkEventButton* event) {
        if (event->type == GDK_BUTTON_PRESS && event->button == 3) { // Tasto destro
            auto selection = m_ipod_view.get_selection();
            if (selection->count_selected_rows() > 0) {
                m_context_view = &m_ipod_view;
                m_context_menu.popup_at_pointer((GdkEvent*)event);
                return true;
            }
        }
        return false;
    }

    void on_menu_rename() {
        LOG(INFO, "Rename menu item clicked");
        if (!m_context_view) return;
        auto selection = m_context_view->get_selection();
        auto iter = selection->get_selected();
        if (!iter) return;
        Gtk::TreeModel::Row row = *iter;
        Glib::ustring filename = row[m_columns.m_col_name];
        Glib::ustring old_path = row[m_columns.m_col_path];
        
        Gtk::Dialog dlg("Rinomina", *this, true);
        Gtk::Entry entry;
        entry.set_text(filename);
        dlg.get_content_area()->pack_start(entry, true, true);
        dlg.add_button("Annulla", Gtk::RESPONSE_CANCEL);
        dlg.add_button("Rinomina", Gtk::RESPONSE_OK);
        dlg.show_all_children();
        
        if (dlg.run() == Gtk::RESPONSE_OK) {
            std::string new_name = entry.get_text();
            if (new_name != filename && !new_name.empty()) {
                std::string context_path;
                if (m_context_view == &m_files_view) {
                    auto folder_sel = m_folder_view.get_selection();
                    auto folder_iter = folder_sel->get_selected();
                    if (folder_iter) {
                        context_path = (Glib::ustring)(*folder_iter)[m_columns.m_col_path];
                    }
                } else {
                    // Folder view: parent path
                    std::string old_path_str = old_path;
                    size_t last_slash = old_path_str.find_last_of('/');
                    if (last_slash != std::string::npos) {
                        context_path = old_path_str.substr(0, last_slash);
                    }
                }

                if (!context_path.empty()) {
                    LOG(DEBUG, "Attempting to rename: " << filename << " to " << new_name << " in context: " << context_path);
                    std::string cmd = "rename \"" + (std::string)filename + "\" \"" + new_name + "\"";
                    exec_smb_command(context_path, m_config, cmd);
                    
                    if (m_context_view == &m_files_view) {
                        on_folder_selected(); // Refresh file list
                        // Update folder view if it's a folder
                        auto folder_sel = m_folder_view.get_selection();
                        auto folder_iter = folder_sel->get_selected();
                        if (folder_iter) {
                             load_folder_content(folder_iter);
                             m_folder_view.expand_row(m_folder_model->get_path(folder_iter), false);
                        }
                    } else {
                        // Folder view rename
                        Gtk::TreeModel::iterator parent = row.parent();
                        if (parent) {
                             load_folder_content(parent);
                             m_folder_view.expand_row(m_folder_model->get_path(parent), false);
                        } else {
                            refresh_list();
                        }
                    }
                } else {
                    LOG(ERROR, "Could not determine context path for rename operation.");
                }
            }
        }
    }

    void on_menu_delete() {
        LOG(INFO, "Delete menu item clicked");
        if (!m_context_view) return;
        auto selection = m_context_view->get_selection();
        auto paths = selection->get_selected_rows();
        if (paths.empty()) return;

        std::string msg = (paths.size() == 1) ? 
            "Sei sicuro di voler eliminare l'elemento selezionato?" : 
            "Sei sicuro di voler eliminare i " + std::to_string(paths.size()) + " elementi selezionati?";

        Gtk::MessageDialog dlg(*this, msg, false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
        if (dlg.run() != Gtk::RESPONSE_YES) return;

        auto model = m_context_view->get_model();
        std::vector<Gtk::TreeRowReference> row_refs;
        for (const auto& path : paths) {
            row_refs.push_back(Gtk::TreeRowReference(model, path));
        }

        bool samba_needs_refresh = false;
        bool samba_folder_deleted = false;

        for (auto& ref : row_refs) {
            if (!ref.is_valid()) continue;
            auto iter = model->get_iter(ref.get_path());
            if (!iter) continue;
            Gtk::TreeModel::Row row = *iter;

            if (m_context_view == &m_ipod_view) {
                std::string file_path = (Glib::ustring)row[m_columns.m_col_path];
                try {
                    if (fs::exists(file_path)) {
                        fs::remove(file_path);
                        m_ipod_model->erase(iter);
                    }
                } catch (const std::exception& e) {
                    LOG(ERROR, "Errore eliminazione iPod: " << e.what());
                }
            } else {
                // Logica Samba
                Glib::ustring filename = row[m_columns.m_col_name];
                Glib::ustring old_path = row[m_columns.m_col_path];
                Glib::ustring icon = row[m_columns.m_col_icon];
                bool is_folder = (icon == "folder");

                std::string context_path;
                if (m_context_view == &m_files_view) {
                    auto folder_sel = m_folder_view.get_selection();
                    auto folder_iter = folder_sel->get_selected();
                    if (folder_iter) context_path = (Glib::ustring)(*folder_iter)[m_columns.m_col_path];
                } else {
                    std::string old_path_str = old_path;
                    size_t last_slash = old_path_str.find_last_of('/');
                    if (last_slash != std::string::npos) context_path = old_path_str.substr(0, last_slash);
                }

                if (!context_path.empty()) {
                    std::string cmd = is_folder ? "rmdir \"" + (std::string)filename + "\"" : "del \"" + (std::string)filename + "\"";
                    auto result = exec_smb_command(context_path, m_config, cmd);
                    if (result.first && result.second.find("NT_STATUS_") == std::string::npos) {
                        if (m_context_view == &m_files_view) m_files_model->erase(iter);
                        else if (m_context_view == &m_folder_view) m_folder_model->erase(iter);
                        samba_needs_refresh = true;
                        if (is_folder) samba_folder_deleted = true;
                    }
                }
            }
        }

        if (m_context_view == &m_files_view) {
            m_pColFile->set_title("File (" + std::to_string(m_files_model->children().size()) + ")");
            if (samba_needs_refresh) {
                Glib::signal_timeout().connect([this](){ on_folder_selected(); return false; }, 200);
                if (samba_folder_deleted) {
                    auto folder_sel = m_folder_view.get_selection();
                    auto folder_iter = folder_sel->get_selected();
                    if (folder_iter) load_folder_content(folder_iter);
                }
            }
        } else if (m_context_view == &m_folder_view && samba_needs_refresh) {
            refresh_list();
        }
    }

    void refresh_list() {
        LOG(INFO, "Refreshing list");
        set_title("Music Network Player (" + m_config.path + ")");
        m_files_model->clear();
        m_folder_model->clear();
        
        LOG(DEBUG, "Starting refresh_list for Samba path: " << m_config.path);
        SambaConfig config = m_config;
        std::thread([this, config](){
            std::string output = exec_smb(config.path, config);
            Glib::signal_idle().connect([this, output](){
                if (output.find("NT_STATUS_") != std::string::npos || (output.find("Connection") != std::string::npos && output.find("failed") != std::string::npos)) {
                    Gtk::MessageDialog dlg(*this, "Errore Samba", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                    dlg.set_secondary_text("Impossibile connettersi: " + output);
                    dlg.run();
                }

                std::istringstream stream(output);
                std::string line;
                while (std::getline(stream, line)) {
                    std::string name;
                    LOG(DEBUG, "Parsing SMB line: " << line);
                    bool is_folder = false;
                    bool is_hidden = false;
                    if (parse_smb_line(line, name, is_folder, is_hidden)) {
                        if (is_folder) {
                            Gtk::TreeModel::Row row = *(m_folder_model->append());
                            row[m_columns.m_col_name] = name;
                            row[m_columns.m_col_icon] = "folder";
                            
                            std::string full_path = m_config.path;
                            if (!full_path.empty() && full_path.back() != '/') full_path += "/";
                            full_path += name;
                            row[m_columns.m_col_path] = full_path;

                            Gtk::TreeModel::Row child = *(m_folder_model->append(row.children()));
                            child[m_columns.m_col_name] = "Loading...";
                        }
                        
                        Gtk::TreeModel::Row row = *(m_files_model->append());
                        set_row_style(row, name, is_folder);
                        if (!is_folder) {
                            std::string full_path = m_config.path;
                            if (!full_path.empty() && full_path.back() != '/') full_path += "/";
                            full_path += name;
                            row[m_columns.m_col_path] = full_path;
                        }
                    }
                }
                m_pColFile->set_title("File (" + std::to_string(m_files_model->children().size()) + ")");
                LOG(DEBUG, "Finished refreshing list. Found " << m_files_model->children().size() << " files.");

                return false;
            });
        }).detach();
    }

    void set_row_style(Gtk::TreeModel::Row& row, const std::string& name, bool is_folder) {
        row[m_columns.m_col_name] = name;
        if (is_folder) {
            row[m_columns.m_col_icon] = "folder";
        } else {
            std::string ext = "";
            size_t dot_pos = name.find_last_of('.');
            if (dot_pos != std::string::npos) {
                ext = name.substr(dot_pos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                row[m_columns.m_col_icon] = "audio-x-generic";
            } else if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" || ext == ".bmp") {
                row[m_columns.m_col_icon] = "image-x-generic";
            } else {
                row[m_columns.m_col_icon] = "text-x-generic";
            }
        }
    }

    void init_db() {
        if (sqlite3_open(DB_FILENAME.c_str(), &m_db)) {
            LOG(ERROR, "Can't open database: " << sqlite3_errmsg(m_db));
            Gtk::MessageDialog dlg(*this, "Errore Database", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK); dlg.set_secondary_text("Impossibile aprire il database: " + std::string(sqlite3_errmsg(m_db))); dlg.run();
            m_db = nullptr;
            return;
        }
        // Aggiungi un timeout per gestire la concorrenza tra thread
        sqlite3_busy_timeout(m_db, 5000); // Attendi fino a 5 secondi
    
        const char* sql = "CREATE TABLE IF NOT EXISTS music ("
                          "path TEXT PRIMARY KEY, "
                          "artist TEXT, title TEXT, album TEXT, year TEXT, genre TEXT, "
                          "coverPath TEXT, bitrate INTEGER, codec TEXT, samplerate INTEGER, size_mb REAL);";
        char* err_msg = 0;
        if (sqlite3_exec(m_db, sql, 0, 0, &err_msg) != SQLITE_OK) {
            LOG(ERROR, "SQL error: " << err_msg);
            Gtk::MessageDialog dlg(*this, "Errore Database", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK); dlg.set_secondary_text("Errore SQL nella creazione tabella: " + std::string(err_msg)); dlg.run();

            sqlite3_free(err_msg);
            sqlite3_close(m_db);
            m_db = nullptr;
            return;
        }

        // Migrazione Schema: Tentiamo di aggiungere le colonne se mancano (per DB vecchi)
        const char* migration_sqls[] = {
            "ALTER TABLE music ADD COLUMN artist TEXT;",
            "ALTER TABLE music ADD COLUMN title TEXT;",
            "ALTER TABLE music ADD COLUMN album TEXT;",
            "ALTER TABLE music ADD COLUMN year TEXT;",
            "ALTER TABLE music ADD COLUMN genre TEXT;",
            "ALTER TABLE music ADD COLUMN coverPath TEXT;",
            "ALTER TABLE music ADD COLUMN bitrate INTEGER;",
            "ALTER TABLE music ADD COLUMN codec TEXT;",
            "ALTER TABLE music ADD COLUMN samplerate INTEGER;",
            "ALTER TABLE music ADD COLUMN size_mb REAL;"
        };

        for (const char* query : migration_sqls) {
            char* err = nullptr;
            sqlite3_exec(m_db, query, 0, 0, &err);
            if (err) sqlite3_free(err); // Ignoriamo errori (es. colonna già esistente)
        }
        LOG(INFO, "Database initialized successfully.");
    }

    void load_db_view() {
        if (!m_db) return;
        m_db_model->clear();
        
        int count = 0;
        LOG(DEBUG, "Loading DB view from database.");
        const char* sql = "SELECT artist, title, album, year, genre, path FROM music ORDER BY artist, album, title;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                count++;
                const unsigned char* artist_unsigned = sqlite3_column_text(stmt, 0);
                const unsigned char* title_unsigned = sqlite3_column_text(stmt, 1);
                const unsigned char* album_unsigned = sqlite3_column_text(stmt, 2);
                const unsigned char* year_unsigned = sqlite3_column_text(stmt, 3);
                const unsigned char* genre_unsigned = sqlite3_column_text(stmt, 4);
                const unsigned char* path_unsigned = sqlite3_column_text(stmt, 5);

                std::string artist = artist_unsigned ? (const char*)artist_unsigned : "";
                std::string title = title_unsigned ? (const char*)title_unsigned : "";
                std::string album = album_unsigned ? (const char*)album_unsigned : "";
                std::string year = year_unsigned ? (const char*)year_unsigned : "";
                std::string genre = genre_unsigned ? (const char*)genre_unsigned : "";
                std::string path = path_unsigned ? (const char*)path_unsigned : "";
                
                Gtk::TreeModel::Row row = *(m_db_model->append());
                if (!artist.empty() && !title.empty()) {
                    row[m_columns.m_col_name] = artist + " - " + title;
                } else {
                    row[m_columns.m_col_name] = fs::path(path).filename().string();
                }
                row[m_columns.m_col_path] = path;
                row[m_columns.m_col_icon] = "audio-x-generic";

                row[m_columns.m_col_artist] = artist;
                row[m_columns.m_col_title] = title;
                row[m_columns.m_col_album] = album;
                row[m_columns.m_col_year] = year;
                row[m_columns.m_col_genre] = genre;
            }
        }
        sqlite3_finalize(stmt);
        LOG(INFO, "DB view loaded. Found " << count << " records.");
        m_scan_status_label.set_text("Record nel DB: " + std::to_string(count));
    }

    void on_clear_db_clicked() {
        Gtk::MessageDialog dlg(*this, "Sei sicuro di voler cancellare l'intero database della libreria?", 
                               false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
        dlg.set_secondary_text("Questa operazione è irreversibile e rimuoverà tutti i brani scansionati.");
        if (dlg.run() != Gtk::RESPONSE_YES) {
            return;
        }

        if (m_db) {
            char* err_msg = 0;
            if (sqlite3_exec(m_db, "DELETE FROM music;", 0, 0, &err_msg) != SQLITE_OK) {
                LOG(ERROR, "SQL error on DELETE: " << err_msg);
                Gtk::MessageDialog err_dlg(*this, "Errore Database", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK); err_dlg.set_secondary_text("Errore SQL nella pulizia: " + std::string(err_msg)); err_dlg.run();
                sqlite3_free(err_msg);
            } else {
                sqlite3_exec(m_db, "VACUUM;", 0, 0, &err_msg); // Best effort
                LOG(INFO, "Database cleared successfully.");
                load_db_view(); // Refresh the view
            }
        }
    }

    void on_scan_library_clicked() {
        LOG(INFO, "Scan library button clicked.");
        if (m_scan_running) {
            m_stop_scan = true;
            m_btnScanDB.set_sensitive(false);
            m_btnScanDB.set_label("Interruzione...");
            return;
        }

        if (m_scan_thread.joinable()) {
            m_scan_thread.join(); // Join the previously finished thread
        }

        m_scan_running = true;
        m_stop_scan = false;
        m_btnScanDB.set_label("Ferma Scansione");
        m_scan_status_label.set_text("");
        m_scan_progress_bar.show();
        m_scan_progress_bar.set_text("Fase 1: Ricerca file...");
        m_scan_progress_bar.set_show_text(true);
        m_scan_progress_bar.set_fraction(0.0);
        
        auto phase1_active = std::make_shared<std::atomic<bool>>(true);

        m_scan_thread = std::thread([this, phase1_active]() {
            LOG(INFO, "Scan library thread started.");
            auto thread_cleanup = [this](const std::string& final_message) {
                m_scan_running = false;
                Glib::signal_idle().connect([this, final_message]() {
                    m_btnScanDB.set_label("Scansiona Libreria");
                    m_btnScanDB.set_sensitive(true);
                    if (final_message == "Scansione completata.") {
                        m_scan_progress_bar.set_fraction(1.0);
                        m_scan_progress_bar.set_text("Completato!");
                    } else {
                        m_scan_progress_bar.set_text(final_message);
                    }
                    
                    if (m_db_view_active) {
                        load_db_view();
                    }
                    Glib::signal_timeout().connect([this]() {
                        m_scan_progress_bar.hide();
                        if (!m_db_view_active) m_scan_status_label.set_text("");
                        return false;
                    }, 2000);
                    return false;
                });
            };

            std::vector<std::string> audio_files;
            LOG(DEBUG, "Phase 1: Starting recursive search for audio files in: " << m_config.path);
            // Fase 1: Trova tutti i file audio ricorsivamente
            Glib::signal_idle().connect([this, phase1_active]() {
                if (m_scan_running && !m_stop_scan && *phase1_active) {
                    m_scan_progress_bar.pulse();
                    return true; // Continue pulsing
                }
                return false;
            });
            find_audio_files_recursive(m_config.path, audio_files);
            LOG(DEBUG, "Phase 1 completed. Found " << audio_files.size() << " audio files.");
            *phase1_active = false; // Ferma l'animazione pulsante

            // Fase 2: Processa ogni file
            int count = 0;
            int total = audio_files.size();
            const int BATCH_SIZE = 100; // Aumentato per ridurre overhead disco
            
            // Avvia la prima transazione
            sqlite3_stmt* stmt_check = nullptr;
            sqlite3_stmt* stmt_insert = nullptr;

            if (m_db) {
                LOG(DEBUG, "Starting database transaction for scan.");
                sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, 0);
                // Prepara gli statement una sola volta fuori dal ciclo
                sqlite3_prepare_v2(m_db, "SELECT 1 FROM music WHERE path = ? AND size_mb IS NOT NULL;", -1, &stmt_check, 0);
                const char* sql_insert = "INSERT OR REPLACE INTO music (path, artist, title, album, year, genre, coverPath, bitrate, codec, samplerate, size_mb) "
                                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
                sqlite3_prepare_v2(m_db, sql_insert, -1, &stmt_insert, 0);
            }

            for (const auto& file_path : audio_files) {
                if (m_stop_scan) {
                    // Se interrotto, SALVA (COMMIT) quello che abbiamo fatto finora invece di buttare tutto
                    LOG(INFO, "Scan interrupted by user. Committing current changes.");
                    if (stmt_check) sqlite3_finalize(stmt_check);
                    if (stmt_insert) sqlite3_finalize(stmt_insert);
                    if (m_db) sqlite3_exec(m_db, "COMMIT;", 0, 0, 0);
                    thread_cleanup("Scansione interrotta (Dati salvati).");
                    return;
                }
                
                count++;
                std::string filename = fs::path(file_path).filename();
                double fraction = total > 0 ? (double)count / total : 0.0;
                
                // Riduci la frequenza degli aggiornamenti UI per non bloccare l'interfaccia
                if (count % 50 == 0 || count == total) {
                    Glib::signal_idle().connect([this, count, total, fraction]() {
                        std::string text = std::to_string(count) + " / " + std::to_string(total);
                        m_scan_progress_bar.set_fraction(fraction);
                        m_scan_progress_bar.set_text(text);
                        return false;
                    });
                }

                // Se il file è già nel DB, saltalo per velocizzare le ri-scansioni
                bool exists_in_db = false;
                if (stmt_check) {
                    sqlite3_reset(stmt_check);
                    sqlite3_clear_bindings(stmt_check);
                    sqlite3_bind_text(stmt_check, 1, file_path.c_str(), -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt_check) == SQLITE_ROW) {
                        exists_in_db = true;
                        LOG(DEBUG, "File already in DB and has metadata: " << file_path << ". Skipping.");
                    }
                }
                if (exists_in_db) continue;

                std::string ext = fs::path(file_path).extension();
                // USARE FILE TEMPORANEO SU DISCO (Più affidabile di RAM/Pipe)
                // Using a rotating temp file name to avoid conflicts if previous cleanup failed
                // and to potentially spread disk I/O if /tmp is on a slow device.
                // The modulo 5 is arbitrary, just to cycle through a few names.
                std::string temp_path = "/tmp/scan_temp_" + std::to_string(count % 5) + ext;
                if (fs::exists(temp_path)) fs::remove(temp_path);
                
                if (download_smb_file(file_path, temp_path, m_config)) {
                    Metadata meta = extract_metadata_internal(temp_path);
                    
                    // Gestione copertina estratta su file
                    if (!meta.coverPath.empty() && fs::exists(meta.coverPath)) {
                        std::string cache_cover = get_hashed_path(file_path, ".jpg");
                        try {
                            LOG(DEBUG, "Copying extracted cover from " << meta.coverPath << " to cache: " << cache_cover);
                            fs::copy_file(meta.coverPath, cache_cover, fs::copy_options::overwrite_existing);
                            meta.coverPath = cache_cover;
                        } catch(...) {}
                        if (fs::exists(meta.coverPath)) { // Check again in case copy failed
                            fs::remove(meta.coverPath); // Rimuovi cover temp
                            LOG(DEBUG, "Removed temporary cover file: " << meta.coverPath);
                        }
                    }
                    fs::remove(temp_path); // Rimuovi audio temp

                    if (stmt_insert) {
                        sqlite3_reset(stmt_insert);
                        sqlite3_clear_bindings(stmt_insert);
                        sqlite3_bind_text(stmt_insert, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 2, sanitize_utf8(meta.artist).c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 3, sanitize_utf8(meta.title).c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 4, sanitize_utf8(meta.album).c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 5, sanitize_utf8(meta.year).c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 6, sanitize_utf8(meta.genre).c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt_insert, 7, meta.coverPath.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(stmt_insert, 8, meta.bitrate);
                        sqlite3_bind_text(stmt_insert, 9, meta.codec.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(stmt_insert, 10, meta.samplerate);
                        sqlite3_bind_double(stmt_insert, 11, meta.size_mb);
                        
                        if (sqlite3_step(stmt_insert) != SQLITE_DONE) {
                            LOG(ERROR, "Failed to insert/update DB for " << file_path << ": " << sqlite3_errmsg(m_db));
                        }
                    }
                } else {
                    LOG(ERROR, "Download failed during scan for: " << file_path);
                }

                // Gestione salvataggio a blocchi (Batch Commit)
                if (count % BATCH_SIZE == 0) {
                    if (m_db) {
                        // Chiudi transazione corrente (salva)
                        sqlite3_exec(m_db, "COMMIT;", 0, 0, 0);
                        LOG(DEBUG, "Committed batch of " << BATCH_SIZE << " files.");
                        
                        // Riapri nuova transazione
                        sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, 0);
                    }
                }
            }

            if (stmt_check) sqlite3_finalize(stmt_check);
            if (stmt_insert) sqlite3_finalize(stmt_insert);

            // Finalizza la transazione
            if (m_db) {
                sqlite3_exec(m_db, "COMMIT;", 0, 0, 0);
                LOG(INFO, "Scansione completata e salvata.");
            }

            thread_cleanup(m_stop_scan ? "Scansione interrotta." : "Scansione completata.");
        });
    }

    void find_audio_files_recursive(const std::string& path, std::vector<std::string>& audio_files) {
        if (m_stop_scan) return;
        LOG(DEBUG, "Scanning directory: " << path);

        // Riduci frequenza aggiornamenti UI
        if (audio_files.size() % 100 == 0) {
            Glib::signal_idle().connect([this, path, count = audio_files.size()]() {
                if (m_scan_running) {
                    m_scan_progress_bar.set_text("Fase 1: Trovati " + std::to_string(count) + " brani...");
                }
                return false;
            });
        }

        std::string output = exec_smb(path, m_config);
        std::istringstream stream(output);
        std::string line;
        
        // Check for SMB errors in the output before parsing lines
        while (std::getline(stream, line)) {
            if (m_stop_scan) return;
            std::string name;
            bool is_folder, is_hidden;
            if (parse_smb_line(line, name, is_folder, is_hidden)) {
                std::string full_path = path;
                if (full_path.back() != '/') full_path += '/';
                full_path += name;

                if (is_folder) {
                    LOG(DEBUG, "Found folder: " << full_path);
                    find_audio_files_recursive(full_path, audio_files);
                } else {
                    std::string ext = fs::path(name).extension();
                    if (!ext.empty() && ext[0] != '.') ext = "." + ext; // Ensure dot prefix
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                        audio_files.push_back(full_path);
                    }
                }
            }
        }
    }

    void update_db_labels(const std::string& path) {
        std::string artist, title, album, year, genre;
        bool found = false;

        LOG(DEBUG, "Updating DB labels for path: " << path);
        if (m_db) {
            const char* sql = "SELECT artist, title, album, year, genre FROM music WHERE path = ?;";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    auto get_text = [&](int col) {
                        const unsigned char* t = sqlite3_column_text(stmt, col);
                        return t ? std::string((const char*)t) : std::string("");
                    };
                    artist = get_text(0);
                    title = get_text(1);
                    album = get_text(2);
                    year = get_text(3);
                    genre = get_text(4);
                    found = true;
                }
                else { LOG(DEBUG, "No DB entry found for path: " << path); }
                sqlite3_finalize(stmt);
            }
        }
        m_lbl_db_artist.set_text(found && !artist.empty() ? sanitize_utf8("(DB: " + artist + ")") : "");
        m_lbl_db_title.set_text(found && !title.empty() ? sanitize_utf8("(DB: " + title + ")") : "");
        m_lbl_db_album.set_text(found && !album.empty() ? sanitize_utf8("(DB: " + album + ")") : "");
        m_lbl_db_year.set_text(found && !year.empty() ? sanitize_utf8("(DB: " + year + ")") : "");
        m_lbl_db_genre.set_text(found && !genre.empty() ? sanitize_utf8("(DB: " + genre + ")") : "");
    }

    void on_db_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
        auto iter = m_db_model->get_iter(path);
        if (iter) play_track(iter, m_db_model);
    }

    void on_db_sort_changed() {
        auto selection = m_db_view.get_selection();
        if (auto iter = selection->get_selected()) {
            // Store the unique path of the selected item
            m_selected_db_path_before_sort = (*iter)[m_columns.m_col_path];

            // The sort happens after this handler returns.
            // Schedule a one-time idle callback to re-select the row after sorting.
            Glib::signal_idle().connect([this]() {
                if (m_selected_db_path_before_sort.empty()) {
                    return false;
                }

                // Iterate through the now-sorted model to find the item
                for (const auto& row : m_db_model->children()) {
                    if ((Glib::ustring)row[m_columns.m_col_path] == m_selected_db_path_before_sort) {
                        m_db_view.get_selection()->select(row);
                        m_db_view.scroll_to_row(m_db_model->get_path(row));
                        break;
                    }
                }

                // Clear the stored path and stop the idle callback
                m_selected_db_path_before_sort.clear();
                return false;
            });
        } else {
            m_selected_db_path_before_sort.clear();
        }
    }

protected:
    SambaConfig m_config;
    Gtk::Box m_box; 
    Gtk::Button m_btnConfig, m_btnRefresh, m_btnExit, m_btnCast, m_btn_youtube, m_btnDB, m_btnScanDB, m_btnClearDB, m_btn_save_db_only, m_btnIpod;
    Gtk::ToggleButton m_btnDebugToggle;
    Gtk::Box m_save_buttons_box;
    Gtk::Label m_scan_status_label;
    Gtk::ProgressBar m_scan_progress_bar;
    Gtk::Paned m_middle_pane;
    Gtk::ScrolledWindow m_ipod_scrolled_window;
    Gtk::TreeView m_ipod_view;
    Glib::RefPtr<Gtk::ListStore> m_ipod_model;
    std::string m_current_ipod_path;
    sqlite3* m_db = nullptr;
    std::atomic<bool> m_stop_scan{false};

    static gboolean on_device_monitor_bus_msg_wrapper(GstBus *bus, GstMessage *msg, gpointer user_data) {
        return static_cast<PlayerWindow*>(user_data)->on_device_monitor_bus_msg(bus, msg);
    }

    gboolean on_device_monitor_bus_msg(GstBus *bus, GstMessage *msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_ADDED) {
            GstDevice *device;
            gst_message_parse_device_added(msg, &device);
            
            gchar *name = gst_device_get_display_name(device);
            gchar *klass = gst_device_get_device_class(device);
            
            if (m_current_device_model) {
                auto row = *(m_current_device_model->append());
                row[m_device_columns.m_col_name] = name ? name : "Sconosciuto";
                row[m_device_columns.m_col_class] = klass ? klass : "";
                // Incrementa ref count per conservarlo nel modello
                row[m_device_columns.m_col_device] = (GstDevice*)gst_object_ref(device);
            }
            g_free(name);
            g_free(klass);
        } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_DEVICE_REMOVED) {
            GstDevice *device;
            gst_message_parse_device_removed(msg, &device);
            if (m_current_device_model) {
                auto children = m_current_device_model->children();
                for (auto iter = children.begin(); iter != children.end(); ++iter) {
                    if ((*iter)[m_device_columns.m_col_device] == device) {
                        gst_object_unref(device); // Rilascia il nostro ref
                        m_current_device_model->erase(iter);
                        break;
                    }
                }
            }
        }
        return TRUE;
    }

    std::atomic<bool> m_scan_running{false};
    std::thread m_scan_thread;
    Glib::Dispatcher m_dispatcher_play, m_dispatcher_pause, m_dispatcher_stop, m_dispatcher_next;
    std::thread m_midi_thread;
    // Device Discovery
    DeviceColumns m_device_columns;
    Glib::RefPtr<Gtk::ListStore> m_current_device_model;
    GstDeviceMonitor *m_device_monitor = nullptr;
    // Layout
    Gtk::Box m_top_bar, m_status_bar;
    Gtk::Paned m_main_pane;
    Gtk::Box m_left_container;
    Gtk::Box m_folder_file_pane;
    Gtk::ScrolledWindow m_db_scrolled_window;
    Gtk::TreeView m_db_view;
    Glib::RefPtr<Gtk::ListStore> m_db_model;
    bool m_db_view_active = false;
    Gtk::Menu m_context_menu;
    Gtk::MenuItem m_item_rename, m_item_delete;
    Gtk::ScrolledWindow m_folder_scrolled_window, m_files_scrolled_window, m_col3_scrolled_window;
    Gtk::TreeView m_folder_view;
    Gtk::TreeView m_files_view;
    Gtk::Image m_image_preview;
    Gtk::TextView m_text_view;
    Glib::RefPtr<Gtk::TreeStore> m_folder_model;
    Glib::RefPtr<Gtk::ListStore> m_files_model;
    ModelColumns m_columns;
    GstElement* m_pipeline;
    Gtk::Box m_col3_box;
    Gtk::Box m_audio_controls_box;
    Gtk::Box m_slider_box;
    Gtk::Scale m_scale;
    Gtk::Scale m_volume_scale;
    Gtk::Label m_time_label;
    Gtk::Box m_info_hbox;
    Gtk::Grid m_tech_grid;
    Gtk::Label m_lbl_bitrate, m_lbl_codec, m_lbl_samplerate, m_lbl_size;
    Gtk::Label m_val_bitrate, m_val_codec, m_val_samplerate, m_val_size;
    Gtk::Grid m_metadata_grid;
    Gtk::Label m_lbl_artist, m_lbl_title, m_lbl_album, m_lbl_year, m_lbl_genre;
    Gtk::Entry m_entry_artist, m_entry_title, m_entry_album, m_entry_year, m_entry_genre;
    Gtk::Label m_lbl_db_artist, m_lbl_db_title, m_lbl_db_album, m_lbl_db_year, m_lbl_db_genre;
    Gtk::Button m_btn_save_metadata, m_btn_search_online, m_btn_recognize;
    Gtk::Button m_btn_save_cover;
    Gtk::Box m_btn_box;
    Gtk::Button m_btn_play, m_btn_pause, m_btn_stop;
    bool m_updating_scale = false;
    Gtk::TreeModel::RowReference m_current_track_row;
    int m_search_index = 0;
    std::string m_last_query;
    Gtk::TreeView* m_context_view = nullptr;
    Gtk::TreeViewColumn* m_pColFile;
    Glib::ustring m_selected_db_path_before_sort;
};

int main(int argc, char *argv[]) {
    // Silenzia il warning "Not loading module atk-bridge" se l'accessibilità non è richiesta
    setenv("NO_AT_BRIDGE", "1", 1);

    LOG(INFO, "Starting application");
    gst_init(&argc, &argv);
    auto app = Gtk::Application::create(argc, argv, "org.example.musicplayer");
    PlayerWindow window;
    return app->run(window);
}