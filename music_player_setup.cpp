#include <gtkmm.h>
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

#include <unistd.h> // For readlink
#include <limits.h> // For PATH_MAX
// Richiede C++17
namespace fs = std::filesystem;

#define LOG(severity, message) std::cerr << "[" << #severity << "] " << __FILE__ << ":" << __LINE__ << " " << __FUNCTION__ << "() " << message << std::endl

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
    
    LOG(DEBUG, "Executing command: " << command);

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
};

class PlayerWindow : public Gtk::Window {
public:
    PlayerWindow() : m_pipeline(nullptr), m_scale(Gtk::ORIENTATION_HORIZONTAL), m_volume_scale(Gtk::ORIENTATION_HORIZONTAL), m_selection_gen(0), m_folder_selection_gen(0) {
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
        m_files_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_files_scrolled_window.add(m_files_view);
        m_folder_file_pane.pack_start(m_files_scrolled_window, Gtk::PACK_EXPAND_WIDGET);
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

        // --- Colonna DB ---
        m_db_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        m_db_scrolled_window.add(m_db_view);
        m_db_model = Gtk::ListStore::create(m_columns);
        m_db_view.set_model(m_db_model);
        
        // Configurazione Colonne DB
        m_db_view.append_column("Artista", m_columns.m_col_artist);
        m_db_view.get_column(0)->set_sort_column(m_columns.m_col_artist);
        m_db_view.get_column(0)->set_resizable(true);

        m_db_view.append_column("Titolo", m_columns.m_col_title);
        m_db_view.get_column(1)->set_sort_column(m_columns.m_col_title);
        m_db_view.get_column(1)->set_resizable(true);

        m_db_view.append_column("Album", m_columns.m_col_album);
        m_db_view.get_column(2)->set_sort_column(m_columns.m_col_album);
        m_db_view.get_column(2)->set_resizable(true);

        m_db_view.append_column("Anno", m_columns.m_col_year);
        m_db_view.get_column(3)->set_sort_column(m_columns.m_col_year);
        m_db_view.get_column(3)->set_resizable(true);

        m_db_view.append_column("Genere", m_columns.m_col_genre);
        m_db_view.get_column(4)->set_sort_column(m_columns.m_col_genre);
        m_db_view.get_column(4)->set_resizable(true);

        m_db_view.append_column("Directory", m_columns.m_col_path);
        m_db_view.get_column(5)->set_sort_column(m_columns.m_col_path);
        m_db_view.get_column(5)->set_resizable(true);

        m_db_view.signal_row_activated().connect(sigc::mem_fun(*this, &PlayerWindow::on_db_row_activated));
        m_db_model->set_sort_column(m_columns.m_col_artist, Gtk::SORT_ASCENDING);


        m_left_container.pack_start(m_folder_file_pane, Gtk::PACK_EXPAND_WIDGET);
        m_left_container.pack_start(m_db_scrolled_window, Gtk::PACK_EXPAND_WIDGET);


        // --- Colonna 3: Placeholder ---
        m_col3_scrolled_window.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        
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

        m_btn_search_online.set_label("Cerca Online");
        m_btn_search_online.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_search_online_clicked));
        m_metadata_grid.attach(m_btn_search_online, 0, 5, 2, 1);

        m_btn_save_metadata.set_label("Salva Metadati");
        m_btn_save_metadata.signal_clicked().connect(sigc::mem_fun(*this, &PlayerWindow::on_save_metadata_clicked));
        m_metadata_grid.attach(m_btn_save_metadata, 0, 6, 2, 1);

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
    }

    ~PlayerWindow() override {
        m_stop_scan = true;
        if (m_scan_thread.joinable()) m_scan_thread.join();

        save_meta_cache();
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
        }
        if (m_db) {
            sqlite3_close(m_db);
        }
    }

    void on_db_view_toggled() {
        m_db_view_active = !m_db_view_active;
        if (m_db_view_active) {
            m_folder_file_pane.hide();
            m_db_scrolled_window.show();
            m_btnDB.set_label("Cartelle");

            // Popola la vista DB dal database SQLite
            load_db_view();
        } else {
            m_db_scrolled_window.hide();
            m_folder_file_pane.show();
            m_btnDB.set_label("DB");
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
        auto selection = m_files_view.get_selection();
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

        // Pulisci eventuale copertina temporanea precedente
        if (fs::exists(get_cache_dir() + "/temp_cover.jpg")) {
            fs::remove("/tmp/temp_cover.jpg");
        }

        if (iter) {
            Gtk::TreeModel::Row row = *iter;
            Glib::ustring name = row[m_columns.m_col_name];
            Glib::ustring path = row[m_columns.m_col_path];
            
          int gen = ++m_selection_gen;

            std::string ext = "";
            size_t dot_pos = name.find_last_of('.');
            if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" || ext == ".bmp") {
                std::string cache_path = get_hashed_path(path, ext);
                if (fs::exists(cache_path)) {
                    display_image(cache_path);
                } else {
                    std::thread([this, path, cache_path, gen]() {
                        if (download_smb_file(path, cache_path, m_config)) {
                            Glib::signal_idle().connect([this, cache_path, gen]() {
                                if (m_selection_gen == gen) {
                                    display_image(cache_path);
                                }
                                return false;
                            });
                        }
                    }).detach();
                }
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                bool need_extract = true;
                if (m_meta_cache.count((std::string)path)) {
                    Metadata meta = m_meta_cache[(std::string)path];
                    if (meta.size_mb > 0.0) {
                        update_metadata_ui(meta);
                        need_extract = false;
                    }
                }
                if (need_extract) {
                    std::thread([this, path, ext, gen]() {
                        std::string temp_path = "/tmp/metadata_preview_" + std::to_string(gen) + ext;
                        if (download_smb_file(path, temp_path, m_config)) {
                            Metadata meta = extract_metadata_internal(temp_path);
                            Glib::signal_idle().connect([this, meta, path, gen]() {
                                if (m_selection_gen == gen) {
                                    // Gestione cache copertina
                                    Metadata final_meta = meta;
                                    if (!final_meta.coverPath.empty() && fs::exists(final_meta.coverPath)) {
                                        std::string cache_cover = get_hashed_path(path, ".jpg");
                                        // Sposta o sovrascrivi in cache
                                        fs::copy_file(final_meta.coverPath, cache_cover, fs::copy_options::overwrite_existing);
                                        fs::remove(final_meta.coverPath);
                                        final_meta.coverPath = cache_cover;
                                    }
                                    
                                    update_metadata_ui(final_meta);
                                    m_meta_cache[(std::string)path] = final_meta;
                                }
                                return false;
                            });
                            fs::remove(temp_path);
                        }
                    }).detach();
                }
            } else {
                std::string cache_path = get_hashed_path(path, ext);
                if (fs::exists(cache_path)) {
                    display_text(cache_path);
                } else {
                    std::thread([this, path, cache_path, gen]() {
                        if (download_smb_file(path, cache_path, m_config)) {
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
            m_text_view.get_buffer()->set_text(buffer.str());
            m_text_view.show();
        } catch(...) {}
    }

    void update_metadata_ui(const Metadata& meta) {
        m_audio_controls_box.show();
        m_info_hbox.show_all();
        m_entry_artist.set_text(meta.artist);
        m_entry_title.set_text(meta.title);
        m_entry_album.set_text(meta.album);
        m_entry_year.set_text(meta.year);
        m_entry_genre.set_text(meta.genre);

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

    void on_file_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* /*column*/) {
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
            play_track(iter);
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
                            // Handle error
                            break;
                        case GST_MESSAGE_TAG: {
                            GstTagList *tags = nullptr;
                            gst_message_parse_tag(msg, &tags);
                            update_metadata_from_tags(tags);
                            
                            // Aggiorna cache durante la riproduzione
                            if (m_current_track_row.is_valid()) {
                                auto iter = m_files_model->get_iter(m_current_track_row.get_path());
                                if (iter) {
                                    Glib::ustring path = (*iter)[m_columns.m_col_path];
                                    Metadata meta;
                                    meta.artist = m_entry_artist.get_text();
                                    meta.title = m_entry_title.get_text();
                                    meta.album = m_entry_album.get_text();
                                    meta.year = m_entry_year.get_text();
                                    meta.genre = m_entry_genre.get_text();
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
        return true;
    }

    void play_track(const Gtk::TreeModel::iterator& iter) {
        if (!iter) return;
        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring smb_path = row[m_columns.m_col_path];

        set_title(name);

        if (m_meta_cache.count((std::string)smb_path)) {
            update_metadata_ui(m_meta_cache[(std::string)smb_path]);
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
        }

        std::string ext = "";
        size_t dot_pos = name.find_last_of('.');
        if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
            if (!m_pipeline) {
                m_pipeline = gst_element_factory_make("playbin", "playbin");
            } else {
                gst_element_set_state(m_pipeline, GST_STATE_NULL);
            }

            if (m_pipeline) {
                std::string local_path = "/tmp/current_track" + ext;
                if (download_smb_file(smb_path, local_path, m_config)) {
                    std::string uri = "file://" + local_path;
                    g_object_set(m_pipeline, "uri", uri.c_str(), NULL);
                    g_object_set(m_pipeline, "volume", m_volume_scale.get_value() / 100.0, NULL);
                    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                    
                    m_current_track_row = Gtk::TreeModel::RowReference(m_files_model, Gtk::TreeModel::Path(iter));
                    m_files_view.get_selection()->select(iter);
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
        auto path = m_current_track_row.get_path();
        auto iter = m_files_model->get_iter(path);
        
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
                play_track(iter);
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
                    if (width > 20) {
                        int new_width = width - 20;
                        int new_height = (pixbuf->get_height() * new_width) / pixbuf->get_width();
                        pixbuf = pixbuf->scale_simple(new_width, new_height, Gdk::INTERP_BILINEAR);
                    }
                    m_image_preview.set(pixbuf);
                    m_image_preview.show();
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
                    if (width > 20) {
                        int new_width = width - 20;
                        int new_height = (pixbuf->get_height() * new_width) / pixbuf->get_width();
                        pixbuf = pixbuf->scale_simple(new_width, new_height, Gdk::INTERP_BILINEAR);
                    }
                    m_image_preview.set(pixbuf);
                    m_image_preview.show();
                } catch(...) {}
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
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
            gchar *artist = nullptr, *title = nullptr, *album = nullptr, *genre = nullptr;
            GstDateTime *date = nullptr;

            if (gst_tag_list_get_string(final_tags, GST_TAG_ARTIST, &artist)) {
                meta.artist = artist ? artist : "";
                g_free(artist);
            }
            if (gst_tag_list_get_string(final_tags, GST_TAG_TITLE, &title)) {
                meta.title = title ? title : "";
                g_free(title);
            }
            if (gst_tag_list_get_string(final_tags, GST_TAG_ALBUM, &album)) {
                meta.album = album ? album : "";
                g_free(album);
            }
            if (gst_tag_list_get_string(final_tags, GST_TAG_GENRE, &genre)) {
                meta.genre = genre ? genre : "";
                g_free(genre);
            }
            if (gst_tag_list_get_date_time(final_tags, GST_TAG_DATE_TIME, &date)) {
                if (gst_date_time_has_year(date)) {
                    meta.year = std::to_string(gst_date_time_get_year(date));
                }
                gst_date_time_unref(date);
            }

            guint bitrate = 0;
            if (gst_tag_list_get_uint(final_tags, GST_TAG_BITRATE, &bitrate)) {
                meta.bitrate = bitrate;
            } else if (gst_tag_list_get_uint(final_tags, GST_TAG_NOMINAL_BITRATE, &bitrate)) {
                meta.bitrate = bitrate;
            }
            gchar *codec = nullptr;
            if (gst_tag_list_get_string(final_tags, GST_TAG_AUDIO_CODEC, &codec)) {
                meta.codec = codec ? codec : "";
                g_free(codec);
            }

            // Estrai copertina e salva su file temporaneo
            GstSample *sample = nullptr;
            if (gst_tag_list_get_sample(final_tags, GST_TAG_IMAGE, &sample) || 
                gst_tag_list_get_sample(final_tags, GST_TAG_PREVIEW_IMAGE, &sample)) {
                GstBuffer *buffer = gst_sample_get_buffer(sample);
                GstMapInfo map;
                if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    std::string cover_fn = local_path + ".cover.jpg";
                    std::ofstream f(cover_fn, std::ios::binary);
                    f.write((char*)map.data, map.size);
                    f.close();
                    meta.coverPath = cover_fn;
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            }

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
                uint64_t size_bytes = fs::file_size(local_path);
                double seconds = (double)dur / GST_SECOND;
                meta.bitrate = (unsigned int)((size_bytes * 8) / seconds);
            }
        }

        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return meta;
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
        auto selection = m_files_view.get_selection();
        auto iter = selection->get_selected();
        if (!iter) return;

        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring smb_path = row[m_columns.m_col_path];

        std::string ext = "";
        size_t dot_pos = name.find_last_of('.');
        if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
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
                std::string exe_path = (count > 0) ? self_path_buf : "";
                std::string script_path = fs::path(exe_path).parent_path().string() + "/google_recognize.sh";
                
                std::string cmd = "bash \"" + script_path + "\" \"" + local_path + "\" 2>&1";
                
                std::array<char, 256> buffer; // Larger buffer
                std::string result;
                std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
                
                if (pipe) {
                    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                        result += buffer.data();
                    }
                }

                Glib::signal_idle().connect([this, result, local_path, smb_path]() mutable {
                    m_btn_recognize.set_sensitive(true);
                    m_btn_recognize.set_label("Invia a SongRep"); // Restore label
                    if (fs::exists(local_path)) fs::remove(local_path);

                    // Verifica che la selezione non sia cambiata nel frattempo
                    auto selection = m_files_view.get_selection();
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

    void perform_save_metadata(bool silent) {
        LOG(INFO, "Save metadata clicked");
        auto selection = m_files_view.get_selection();
        auto iter = selection->get_selected();
        if (!iter) return;

        Gtk::TreeModel::Row row = *iter;
        Glib::ustring name = row[m_columns.m_col_name];
        Glib::ustring smb_path = row[m_columns.m_col_path];

        std::string ext = "";
        size_t dot_pos = name.find_last_of('.');
        if (dot_pos != std::string::npos) ext = name.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        // Supporto scrittura solo per MP3 per ora
        if (ext != ".mp3") {
            if (!silent) {
                Gtk::MessageDialog dlg(*this, "Modifica supportata solo per file MP3", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK);
                dlg.run();
            }
            return;
        }

        std::string local_path = "/tmp/current_track" + ext;
        std::string temp_out_path = "/tmp/temp_tag_write" + ext;

        // Pipeline per scrivere i tag: filesrc -> mpegaudioparse -> id3mux -> filesink
        GstElement *pipeline = gst_pipeline_new("tag-writer");
        GstElement *src = gst_element_factory_make("filesrc", "src");
        GstElement *parse = gst_element_factory_make("mpegaudioparse", "parse");
        GstElement *mux = gst_element_factory_make("id3mux", "mux");
        GstElement *sink = gst_element_factory_make("filesink", "sink");

        if (!pipeline || !src || !parse || !mux || !sink) {
            if (pipeline) gst_object_unref(pipeline);
            return;
        }

        g_object_set(src, "location", local_path.c_str(), NULL);
        g_object_set(sink, "location", temp_out_path.c_str(), NULL);

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
        gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);

        // Sovrascrivi file locale e carica su SMB
        fs::rename(temp_out_path, local_path);
        
        std::string remote_filename = (std::string)name;
        std::string cmd = "put \"" + local_path + "\" \"" + remote_filename + "\"";
        exec_smb_command(smb_path, m_config, cmd);
        
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
                gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
            } else {
                play_track(iter);
            }
        } else if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        }
    }

    void on_pause_clicked() {
        if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    }

    void on_stop_clicked() {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            m_updating_scale = true;
            m_scale.set_value(0);
            m_updating_scale = false;
            set_title("Music Network Player " + APP_VERSION);
        }
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
                }
            }
        }
    }

    void on_menu_delete() {
        LOG(INFO, "Delete menu item clicked");
        LOG(INFO, "Delete menu item clicked");
        if (!m_context_view) return;
        auto selection = m_context_view->get_selection();

        auto iter = selection->get_selected();
        if (!iter) return;
        Gtk::TreeModel::Row row = *iter;
        Glib::ustring filename = row[m_columns.m_col_name];

        Glib::ustring old_path = row[m_columns.m_col_path];
        Glib::ustring icon = row[m_columns.m_col_icon];
        bool is_folder = (icon == "folder");

        Gtk::MessageDialog dlg(*this, "Sei sicuro di voler eliminare " + filename + "?", false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_YES_NO);
        if (dlg.run() == Gtk::RESPONSE_YES) {
             std::string context_path;
             if (m_context_view == &m_files_view) {
                 auto folder_sel = m_folder_view.get_selection();

                 auto folder_iter = folder_sel->get_selected();
                 if (folder_iter) {
                     context_path = (Glib::ustring)(*folder_iter)[m_columns.m_col_path];
                 }
             } else {
                 std::string old_path_str = old_path;
                 size_t last_slash = old_path_str.find_last_of('/');
                 if (last_slash != std::string::npos) {
                     context_path = old_path_str.substr(0, last_slash);
                 }
             }

             if (!context_path.empty()) {
                std::string full_smb_path = context_path;
                if (!full_smb_path.empty() && full_smb_path.back() != '/') full_smb_path += "/";
                full_smb_path += filename;

                 std::string cmd;
                 if (is_folder) {
                     cmd = "rmdir \"" + (std::string)filename + "\"";
                 } else {
                     cmd = "del \"" + (std::string)filename + "\"";
                 }

                 std::pair<bool, std::string> result = exec_smb_command(context_path, m_config, cmd);
                 bool deleted = false;
                 std::string error_message;

                 // smbclient può restituire 0 anche se il comando fallisce (es. rmdir su cartella non vuota),
                 // quindi controlliamo l'output per i messaggi di stato NT.
                 if (result.first && result.second.find("NT_STATUS_") == std::string::npos) {
                     deleted = true;
                 } else {
                     error_message = result.second;
                 }
 
                 if (!deleted) {
                     std::cerr << "Errore durante l'eliminazione di " << filename << ": " << error_message << std::endl;
                     Gtk::MessageDialog dlg(*this, "Errore durante l'eliminazione: " + error_message, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                     dlg.run();                    
                        return;
                    }
                 
                 if (m_context_view == &m_files_view) {
                     m_files_model->erase(iter);
                     
                    
                    
                    if (!error_message.empty()) {
                      std::cerr << "Output from smbclient: " << error_message << std::endl;
                    }

                    
                     m_pColFile->set_title("File (" + std::to_string(m_files_model->children().size()) + ")");

                     Glib::signal_timeout().connect([this](){ on_folder_selected(); return false; }, 200);
                     if (is_folder) {
                         auto folder_sel = m_folder_view.get_selection();
                         auto folder_iter = folder_sel->get_selected();
                         if (folder_iter) {
                             load_folder_content(folder_iter);
                             m_folder_view.expand_row(m_folder_model->get_path(folder_iter), false);
                         }
                     }
                 } else {
                     Gtk::TreeModel::iterator parent = row.parent();
                     if (parent) {
                         load_folder_content(parent);
                         m_folder_view.expand_row(m_folder_model->get_path(parent), false);
                     } else {
                         refresh_list();
                     }
                 }
             }
        }
    }

    void refresh_list() {
        LOG(INFO, "Refreshing list");
        set_title("Music Network Player (" + m_config.path + ")");
        m_files_model->clear();
        m_folder_model->clear();
        
        SambaConfig config = m_config;
        std::thread([this, config](){
            std::string output = exec_smb(config.path, config);
            Glib::signal_idle().connect([this, output](){
                std::istringstream stream(output);
                std::string line;
                while (std::getline(stream, line)) {
                    std::string name;
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
            sqlite3_free(err_msg);
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }

    void load_db_view() {
        if (!m_db) return;
        m_db_model->clear();
        
        const char* sql = "SELECT artist, title, album, year, genre, path FROM music ORDER BY artist, album, title;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
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
                row[m_columns.m_col_name] = artist + " - " + title;
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
                sqlite3_free(err_msg);
                Gtk::MessageDialog err_dlg(*this, "Errore durante la pulizia del database.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
                err_dlg.run();
            } else {
                sqlite3_exec(m_db, "VACUUM;", 0, 0, &err_msg); // Best effort
                LOG(INFO, "Database cleared successfully.");
                load_db_view(); // Refresh the view
            }
        }
    }

    void on_scan_library_clicked() {
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
                        m_scan_status_label.set_text("");
                        return false;
                    }, 2000);
                    return false;
                });
            };

            std::vector<std::string> audio_files;
            // Fase 1: Trova tutti i file audio ricorsivamente
            Glib::signal_idle().connect([this, phase1_active]() {
                if (m_scan_running && !m_stop_scan && *phase1_active) {
                    m_scan_progress_bar.pulse();
                    return true; // Continue pulsing
                }
                return false;
            });
            find_audio_files_recursive(m_config.path, audio_files);
            *phase1_active = false; // Ferma l'animazione pulsante

            // Fase 2: Processa ogni file
            int count = 0;
            int total = audio_files.size();
            const int BATCH_SIZE = 5; // Salva ogni 5 brani
            
            // Avvia la prima transazione
            if (m_db) sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, 0);

            for (const auto& file_path : audio_files) {
                if (m_stop_scan) {
                    // Se interrotto, SALVA (COMMIT) quello che abbiamo fatto finora invece di buttare tutto
                    if (m_db) sqlite3_exec(m_db, "COMMIT;", 0, 0, 0);
                    thread_cleanup("Scansione interrotta (Dati salvati).");
                    return;
                }
                
                count++;
                std::string filename = fs::path(file_path).filename();
                double fraction = total > 0 ? (double)count / total : 0.0;
                
                Glib::signal_idle().connect([this, count, total, fraction]() {
                    std::string text = std::to_string(count) + " / " + std::to_string(total);
                    m_scan_progress_bar.set_fraction(fraction);
                    m_scan_progress_bar.set_text(text);
                    return false;
                });

                // Se il file è già nel DB, saltalo per velocizzare le ri-scansioni
                bool exists_in_db = false;
                if (m_db) {
                    sqlite3_stmt* stmt;
                    const char* sql = "SELECT 1 FROM music WHERE path = ?;";
                    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, 0) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, file_path.c_str(), -1, SQLITE_STATIC);
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            exists_in_db = true;
                        }
                        sqlite3_finalize(stmt);
                    }
                }
                if (exists_in_db) continue;

                std::string ext = fs::path(file_path).extension();
                std::string temp_path = "/tmp/scan_temp_" + std::to_string(count) + ext;
                if (download_smb_file(file_path, temp_path, m_config)) {
                    Metadata meta = extract_metadata_internal(temp_path);
                    
                    // Metti in cache la copertina se estratta
                    if (!meta.coverPath.empty() && fs::exists(meta.coverPath)) {
                        std::string cache_cover_path = get_hashed_path(file_path, ".jpg");
                        try {
                            fs::copy_file(meta.coverPath, cache_cover_path, fs::copy_options::overwrite_existing);
                            fs::remove(meta.coverPath);
                            meta.coverPath = cache_cover_path;
                        } catch (const fs::filesystem_error& e) {
                            LOG(ERROR, "Failed to cache cover: " << e.what());
                            meta.coverPath = "";
                        }
                    }

                    if (m_db) {
                        const char* sql_insert = "INSERT OR REPLACE INTO music (path, artist, title, album, year, genre, coverPath, bitrate, codec, samplerate, size_mb) "
                                                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
                        sqlite3_stmt* stmt_insert;
                        if (sqlite3_prepare_v2(m_db, sql_insert, -1, &stmt_insert, 0) == SQLITE_OK) {
                            sqlite3_bind_text(stmt_insert, 1, file_path.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 2, meta.artist.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 3, meta.title.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 4, meta.album.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 5, meta.year.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 6, meta.genre.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(stmt_insert, 7, meta.coverPath.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int(stmt_insert, 8, meta.bitrate);
                            sqlite3_bind_text(stmt_insert, 9, meta.codec.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int(stmt_insert, 10, meta.samplerate);
                            sqlite3_bind_double(stmt_insert, 11, meta.size_mb);
                            
                            if (sqlite3_step(stmt_insert) != SQLITE_DONE) {
                                // Errore di inserimento silenziato per non mostrare nomi file nella shell.
                            }
                            sqlite3_finalize(stmt_insert);
                        }
                    }
                    fs::remove(temp_path);
                }

                // Gestione salvataggio a blocchi (Batch Commit)
                if (count % BATCH_SIZE == 0) {
                    if (m_db) {
                        // Chiudi transazione corrente (salva)
                        sqlite3_exec(m_db, "COMMIT;", 0, 0, 0);
                        
                        // Aggiorna la vista utente
                        Glib::signal_idle().connect([this]() {
                            if (m_db_view_active) load_db_view();
                            return false;
                        });

                        // Riapri nuova transazione
                        sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0, 0, 0);
                    }
                }
            }

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

        // Aggiorna la barra di stato per mostrare che stiamo lavorando
        Glib::signal_idle().connect([this, path, count = audio_files.size()]() {
            if (m_scan_running) {
                m_scan_progress_bar.set_text("Fase 1: Trovati " + std::to_string(count) + " brani...");
            }
            return false;
        });

        std::string output = exec_smb(path, m_config);
        std::istringstream stream(output);
        std::string line;
        
        while (std::getline(stream, line)) {
            if (m_stop_scan) return;
            std::string name;
            bool is_folder, is_hidden;
            if (parse_smb_line(line, name, is_folder, is_hidden)) {
                std::string full_path = path;
                if (full_path.back() != '/') full_path += '/';
                full_path += name;

                if (is_folder) {
                    find_audio_files_recursive(full_path, audio_files);
                } else {
                    std::string ext = fs::path(name).extension();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
                        audio_files.push_back(full_path);
                    }
                }
            }
        }
    }

    void on_db_row_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn* /*column*/) {
        auto iter = m_db_model->get_iter(path);
        if (!iter) return;
        
        Gtk::TreeModel::Row row = *iter;
        Glib::ustring smb_path = row[m_columns.m_col_path];
        
        std::string name = fs::path((std::string)smb_path).filename();
        set_title(name);

        if (m_meta_cache.count((std::string)smb_path)) {
            update_metadata_ui(m_meta_cache[(std::string)smb_path]);
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
        }

        std::string ext = fs::path(name).extension();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
            if (!m_pipeline) {
                m_pipeline = gst_element_factory_make("playbin", "playbin");
            } else {
                gst_element_set_state(m_pipeline, GST_STATE_NULL);
            }

            if (m_pipeline) {
                std::string local_path = "/tmp/current_track" + ext;
                if (download_smb_file(smb_path, local_path, m_config)) {
                    std::string uri = "file://" + local_path;
                    g_object_set(m_pipeline, "uri", uri.c_str(), NULL);
                    g_object_set(m_pipeline, "volume", m_volume_scale.get_value() / 100.0, NULL);
                    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                    
                    m_current_track_row = Gtk::TreeModel::RowReference();
                    m_db_view.get_selection()->select(iter);
                }
            }
        }
    }

protected:
    SambaConfig m_config;
    Gtk::Box m_box; 
    Gtk::Button m_btnConfig, m_btnRefresh, m_btnExit, m_btnCast, m_btn_youtube, m_btnDB, m_btnScanDB, m_btnClearDB;
    Gtk::Label m_scan_status_label;
    Gtk::ProgressBar m_scan_progress_bar;
    sqlite3* m_db = nullptr;
    std::atomic<bool> m_stop_scan{false};
    std::atomic<bool> m_scan_running{false};
    std::thread m_scan_thread;
    
    // Device Discovery
    DeviceColumns m_device_columns;
    Glib::RefPtr<Gtk::ListStore> m_current_device_model;
    GstDeviceMonitor *m_device_monitor = nullptr;

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
    Gtk::Button m_btn_save_metadata, m_btn_search_online, m_btn_recognize;
    Gtk::Button m_btn_save_cover;
    Gtk::Box m_btn_box;
    Gtk::Button m_btn_play, m_btn_pause, m_btn_stop;
    bool m_updating_scale = false;
    Gtk::TreeModel::RowReference m_current_track_row;
    std::atomic<int> m_selection_gen;
    std::map<std::string, Metadata> m_meta_cache;
    int m_search_index = 0;
    std::string m_last_query;
    Gtk::TreeView* m_context_view = nullptr;
    std::atomic<int> m_folder_selection_gen;
    Gtk::TreeViewColumn* m_pColFile;
};

int main(int argc, char *argv[]) {
    LOG(INFO, "Starting application");
    gst_init(&argc, &argv);
    auto app = Gtk::Application::create(argc, argv, "org.example.musicplayer");
    PlayerWindow window;
    return app->run(window);
}