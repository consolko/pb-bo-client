#pragma once
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <QMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>

// Add each supported language here and embed its Qt catalog in qml/app.qrc.
inline const QMap<QString, QString> &interfaceLanguages() {
    static const QMap<QString, QString> languages{{"en", "English"}, {"ru", "Русский"}};
    return languages;
}

inline QString interfaceLanguage(QString systemLanguage, const QString &preference) {
    if (interfaceLanguages().contains(preference)) return preference;
    systemLanguage = systemLanguage.trimmed().toLower().replace('-', '_');
    // PocketBook uses ISO codes; accept locale names and the older firmware name too.
    if (systemLanguage == "russian") systemLanguage = "ru";
    const auto code = systemLanguage.section('_', 0, 0).section('.', 0, 0).section('@', 0, 0);
    return interfaceLanguages().contains(code) ? code : QStringLiteral("en");
}

inline const QMap<QString, const char *> &messageSources() {
    static const QMap<QString, const char *> sources{
        {"1_files_2_matching_3_different_4_errors_5_unchecked_6_see_book_d_4bf1b3", QT_TRANSLATE_NOOP("BookOrbit", "%1. Files: %2; matching: %3; different: %4; errors: %5; unchecked: %6. See book details for results.")},
        {"1_files_completed_2_of_3_books_synced_4_of_5_needs_attention_6_waiting_7", QT_TRANSLATE_NOOP("BookOrbit", "%1. Files completed: %2 of %3. Books synced: %4 of %5; needs attention: %6; waiting: %7.")},
        {"a_verified_update_was_retained_retry_installation_when_ready", QT_TRANSLATE_NOOP("BookOrbit", "A verified update was retained. Retry installation when ready.")},
        {"an_application_update_is_available", QT_TRANSLATE_NOOP("BookOrbit", "An application update is available.")},
        {"book_already_downloaded", QT_TRANSLATE_NOOP("BookOrbit", "Book already downloaded")},
        {"book_downloaded_and_available_offline", QT_TRANSLATE_NOOP("BookOrbit", "Book downloaded and available offline")},
        {"cannot_safely_identify_the_running_application_update_installati_7d0fd1", QT_TRANSLATE_NOOP("BookOrbit", "Cannot safely identify the running application. Update installation is blocked.")},
        {"charge_the_battery_to_at_least_30_before_updating", QT_TRANSLATE_NOOP("BookOrbit", "Charge the battery to at least 30% before updating.")},
        {"checking_file_1_of_2_3", QT_TRANSLATE_NOOP("BookOrbit", "Checking file %1 of %2: %3…")},
        {"checking_file_1_of_2_3_4_fileid_5", QT_TRANSLATE_NOOP("BookOrbit", "Checking file %1 of %2: %3 (%4, fileId %5)…")},
        {"checking_the_book_file", QT_TRANSLATE_NOOP("BookOrbit", "Checking the book file…")},
        {"checking_the_current_file_version", QT_TRANSLATE_NOOP("BookOrbit", "Checking the current file version…")},
        {"choose_a_reading_position", QT_TRANSLATE_NOOP("BookOrbit", "Choose a reading position")},
        {"choose_an_accessible_folder_in_internal_storage_outside_system_folders", QT_TRANSLATE_NOOP("BookOrbit", "Choose an accessible folder in internal storage, outside system folders.")},
        {"close_all_books_in_the_built_in_reader_and_retry_sync", QT_TRANSLATE_NOOP("BookOrbit", "Close all books in the built-in reader and retry sync.")},
        {"close_the_book_in_the_built_in_reader_and_retry_sync_the_home_bu_e4edc4", QT_TRANSLATE_NOOP("BookOrbit", "Close the book in the built-in reader and retry sync. The Home button leaves the book open.")},
        {"close_the_book_in_the_built_in_reader_and_retry_the_download", QT_TRANSLATE_NOOP("BookOrbit", "Close the book in the built-in reader and retry the download")},
        {"connect_to_the_network_to_check_updates", QT_TRANSLATE_NOOP("BookOrbit", "Connect to the network to check updates.")},
        {"connect_to_the_network_to_download_the_update", QT_TRANSLATE_NOOP("BookOrbit", "Connect to the network to download the update.")},
        {"connecting", QT_TRANSLATE_NOOP("BookOrbit", "Connecting…")},
        {"connection_saved_downloaded_books_are_available_without_signing_in", QT_TRANSLATE_NOOP("BookOrbit", "Connection saved. Downloaded books are available without signing in.")},
        {"connection_timed_out_check_the_network_and_try_again", QT_TRANSLATE_NOOP("BookOrbit", "Connection timed out. Check the network and try again.")},
        {"could_not_check_the_reader_state_positions_were_kept_unchanged_try_again", QT_TRANSLATE_NOOP("BookOrbit", "Could not check the reader state. Positions were kept unchanged; try again.")},
        {"could_not_check_updates_try_again_later", QT_TRANSLATE_NOOP("BookOrbit", "Could not check updates. Try again later.")},
        {"could_not_confirm_that_the_position_was_saved_retry_sync", QT_TRANSLATE_NOOP("BookOrbit", "Could not confirm that the position was saved. Retry sync.")},
        {"could_not_create_the_app_data_folder_1_check_free_space_and_acce_fb2fd8", QT_TRANSLATE_NOOP("BookOrbit", "Could not create the app data folder: %1. Check free space and access to device storage.")},
        {"could_not_create_the_book_file", QT_TRANSLATE_NOOP("BookOrbit", "Could not create the book file")},
        {"could_not_get_current_file_details", QT_TRANSLATE_NOOP("BookOrbit", "Could not get current file details.")},
        {"could_not_match_the_server_cfi_to_this_book_progress_was_kept_unchanged", QT_TRANSLATE_NOOP("BookOrbit", "Could not match the server CFI to this book. Progress was kept unchanged.")},
        {"could_not_protect_or_remove_sign_in_data_check_device_storage", QT_TRANSLATE_NOOP("BookOrbit", "Could not protect or remove sign-in data. Check device storage.")},
        {"could_not_protect_the_saved_session_sign_in_again", QT_TRANSLATE_NOOP("BookOrbit", "Could not protect the saved session. Sign in again.")},
        {"could_not_protect_the_saved_session_you_will_need_to_sign_in_aga_920b4e", QT_TRANSLATE_NOOP("BookOrbit", "Could not protect the saved session. You will need to sign in again after closing the app.")},
        {"could_not_remove_old_sign_in_data", QT_TRANSLATE_NOOP("BookOrbit", "Could not remove old sign-in data.")},
        {"could_not_remove_the_invalid_session_from_this_device", QT_TRANSLATE_NOOP("BookOrbit", "Could not remove the invalid session from this device.")},
        {"could_not_remove_the_saved_session_check_device_storage", QT_TRANSLATE_NOOP("BookOrbit", "Could not remove the saved session. Check device storage.")},
        {"could_not_save_progress_retry_sync", QT_TRANSLATE_NOOP("BookOrbit", "Could not save progress. Retry sync.")},
        {"could_not_save_the_book", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the book")},
        {"could_not_save_the_book_order", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the book order.")},
        {"could_not_save_the_connection_1", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the connection: %1")},
        {"could_not_save_the_download_journal_the_previous_book_is_still_available", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the download journal. The previous book is still available.")},
        {"could_not_save_the_download_result_the_previous_book_is_still_available", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the download result. The previous book is still available.")},
        {"could_not_save_the_folder_1", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the folder: %1")},
        {"could_not_save_the_language_setting", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the language setting.")},
        {"could_not_save_the_sync_result_resolve_the_storage_error_and_try_again", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the sync result. Resolve the storage error and try again.")},
        {"could_not_save_the_update_download", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the update download.")},
        {"could_not_save_the_verification_result_resolve_the_storage_error_bf59aa", QT_TRANSLATE_NOOP("BookOrbit", "Could not save the verification result. Resolve the storage error and try again.")},
        {"could_not_save_update_settings", QT_TRANSLATE_NOOP("BookOrbit", "Could not save update settings.")},
        {"could_not_start_the_reader", QT_TRANSLATE_NOOP("BookOrbit", "Could not start the reader")},
        {"could_not_verify_the_local_file_changed_during_verification", QT_TRANSLATE_NOOP("BookOrbit", "Could not verify: the local file changed during verification.")},
        {"could_not_verify_the_local_file_is_missing_or_damaged_download_it_again", QT_TRANSLATE_NOOP("BookOrbit", "Could not verify: the local file is missing or damaged. Download it again.")},
        {"could_not_verify_the_server_file", QT_TRANSLATE_NOOP("BookOrbit", "Could not verify the server file.")},
        {"could_not_verify_the_server_tls_certificate_check_the_trusted_ca_3413a3", QT_TRANSLATE_NOOP("BookOrbit", "Could not verify the server TLS certificate. Check the trusted CA and device date.")},
        {"details_received_but_not_saved_for_offline_use", QT_TRANSLATE_NOOP("BookOrbit", "Details received but not saved for offline use")},
        {"details_saved_on_this_device", QT_TRANSLATE_NOOP("BookOrbit", "Details saved on this device")},
        {"diagnostic_log_failed", QT_TRANSLATE_NOOP("BookOrbit", "Could not write diagnostic log. Check device storage.")},
        {"diagnostic_logging_disabled", QT_TRANSLATE_NOOP("BookOrbit", "Diagnostic logging disabled")},
        {"diagnostic_logging_enabled", QT_TRANSLATE_NOOP("BookOrbit", "Diagnostic logging enabled")},
        {"disconnect_usb_before_installing_the_update", QT_TRANSLATE_NOOP("BookOrbit", "Disconnect USB before installing the update.")},
        {"download_a_valid_book_file_before_syncing", QT_TRANSLATE_NOOP("BookOrbit", "Download a valid book file before syncing")},
        {"download_cancelled_the_previous_file_was_kept", QT_TRANSLATE_NOOP("BookOrbit", "Download cancelled. The previous file was kept.")},
        {"download_incomplete_try_again_the_previous_file_was_kept", QT_TRANSLATE_NOOP("BookOrbit", "Download incomplete. Try again; the previous file was kept.")},
        {"download_record_not_saved_the_previous_book_is_still_available", QT_TRANSLATE_NOOP("BookOrbit", "Download record not saved. The previous book is still available.")},
        {"download_the_book_first", QT_TRANSLATE_NOOP("BookOrbit", "Download the book first")},
        {"downloading_book", QT_TRANSLATE_NOOP("BookOrbit", "Downloading book…")},
        {"enter_an_https_address_and_username", QT_TRANSLATE_NOOP("BookOrbit", "Enter an HTTPS address and username.")},
        {"enter_an_https_address_username_and_password", QT_TRANSLATE_NOOP("BookOrbit", "Enter an HTTPS address, username and password.")},
        {"file_downloaded_opening_this_format_from_the_app_is_not_supported_yet", QT_TRANSLATE_NOOP("BookOrbit", "File downloaded. Opening this format from the app is not supported yet.")},
        {"finish_the_current_operation_before_updating", QT_TRANSLATE_NOOP("BookOrbit", "Finish the current operation before updating.")},
        {"folder_saved_previously_downloaded_books_have_stayed_where_they_were", QT_TRANSLATE_NOOP("BookOrbit", "Folder saved. Previously downloaded books have stayed where they were.")},
        {"full_details_not_loaded_yet", QT_TRANSLATE_NOOP("BookOrbit", "Full details not loaded yet")},
        {"installation_stopped_before_replacement_the_prepared_update_was_c3bcc3", QT_TRANSLATE_NOOP("BookOrbit", "Installation stopped before replacement. The prepared update was kept; retry installation.")},
        {"installing_the_update", QT_TRANSLATE_NOOP("BookOrbit", "Installing the update…")},
        {"invalid_archive_checksum", QT_TRANSLATE_NOOP("BookOrbit", "Invalid archive checksum.")},
        {"invalid_book_record", QT_TRANSLATE_NOOP("BookOrbit", "Invalid book record")},
        {"invalid_downloaded_book_record", QT_TRANSLATE_NOOP("BookOrbit", "Invalid downloaded book record")},
        {"invalid_progress_response_expected_a_cfi_and_a_percentage_from_0_2d14d6", QT_TRANSLATE_NOOP("BookOrbit", "Invalid progress response: expected a CFI and a percentage from 0 to 100. Progress was kept unchanged.")},
        {"language_saved", QT_TRANSLATE_NOOP("BookOrbit", "Language saved.")},
        {"loading_catalog", QT_TRANSLATE_NOOP("BookOrbit", "Loading catalog…")},
        {"loading_collections", QT_TRANSLATE_NOOP("BookOrbit", "Loading collections…")},
        {"loading_details", QT_TRANSLATE_NOOP("BookOrbit", "Loading details…")},
        {"logging_is_disabled_could_not_save_the_setting_1", QT_TRANSLATE_NOOP("BookOrbit", "Logging is disabled. Could not save the setting: %1")},
        {"logging_is_enabled_until_the_app_closes_could_not_save_the_setting_1", QT_TRANSLATE_NOOP("BookOrbit", "Logging is enabled until the app closes. Could not save the setting: %1")},
        {"native_position_saving_is_unavailable_for_this_firmware", QT_TRANSLATE_NOOP("BookOrbit", "Native position saving is unavailable for this firmware.")},
        {"network_connected_retry_sync_now", QT_TRANSLATE_NOOP("BookOrbit", "Network connected. Retry sync now.")},
        {"no_books_found", QT_TRANSLATE_NOOP("BookOrbit", "No books found")},
        {"no_downloaded_books_to_sync", QT_TRANSLATE_NOOP("BookOrbit", "No downloaded books to sync")},
        {"no_downloaded_files_to_verify", QT_TRANSLATE_NOOP("BookOrbit", "No downloaded files to verify")},
        {"no_exact_position_on_the_reader_yet", QT_TRANSLATE_NOOP("BookOrbit", "No exact position on the reader yet.")},
        {"not_enough_writable_space_for_the_update", QT_TRANSLATE_NOOP("BookOrbit", "Not enough writable space for the update.")},
        {"not_signed_in_or_session_expired_sign_in_again", QT_TRANSLATE_NOOP("BookOrbit", "Not signed in or session expired. Sign in again.")},
        {"opening_in_the_built_in_reader", QT_TRANSLATE_NOOP("BookOrbit", "Opening in the built-in reader…")},
        {"position_saved_open_the_book_from_the_built_in_library", QT_TRANSLATE_NOOP("BookOrbit", "Position saved. Open the book from the built-in library.")},
        {"position_sync_is_available_for_epub_and_supported_fb2_books_you_9f7a76", QT_TRANSLATE_NOOP("BookOrbit", "Position sync is available for EPUB and supported FB2 books. You can read this file locally.")},
        {"previous_result_unavailable", QT_TRANSLATE_NOOP("BookOrbit", "Previous result details are unavailable. Retry the operation.")},
        {"progress_sent_to_bookorbit", QT_TRANSLATE_NOOP("BookOrbit", "Progress sent to BookOrbit")},
        {"progress_synced", QT_TRANSLATE_NOOP("BookOrbit", "Progress synced")},
        {"progress_unchanged", QT_TRANSLATE_NOOP("BookOrbit", "Progress unchanged")},
        {"refreshing_session", QT_TRANSLATE_NOOP("BookOrbit", "Refreshing session…")},
        {"restoring_session", QT_TRANSLATE_NOOP("BookOrbit", "Restoring session…")},
        {"server_http_error", QT_TRANSLATE_NOOP("BookOrbit", "The server returned HTTP %1. Check the address and try again.")},
        {"server_not_found_1_check_wi_fi_and_dns", QT_TRANSLATE_NOOP("BookOrbit", "Server not found: %1. Check Wi-Fi and DNS.")},
        {"session_expired_sign_in_again_to_download", QT_TRANSLATE_NOOP("BookOrbit", "Session expired. Sign in again to download.")},
        {"session_restored_retry_sync_the_previous_position_was_not_sent_again", QT_TRANSLATE_NOOP("BookOrbit", "Session restored. Retry sync: the previous position was not sent again.")},
        {"session_revoked_downloaded_books_were_kept", QT_TRANSLATE_NOOP("BookOrbit", "Session revoked. Downloaded books were kept.")},
        {"sign_in_to_download", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to download")},
        {"sign_in_to_load_collections", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to load collections")},
        {"sign_in_to_load_full_details", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to load full details")},
        {"sign_in_to_sync", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to sync")},
        {"sign_in_to_the_server_first", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to the server first")},
        {"sign_in_to_verify_library_files", QT_TRANSLATE_NOOP("BookOrbit", "Sign in to verify library files")},
        {"signed_out_on_this_device_downloaded_books_were_kept", QT_TRANSLATE_NOOP("BookOrbit", "Signed out on this device. Downloaded books were kept.")},
        {"signed_out_on_this_device_server_session_revocation_was_not_confirmed", QT_TRANSLATE_NOOP("BookOrbit", "Signed out on this device. Server session revocation was not confirmed.")},
        {"signing_out", QT_TRANSLATE_NOOP("BookOrbit", "Signing out…")},
        {"storage_create_folder", QT_TRANSLATE_NOOP("BookOrbit", "Could not create folder %1.")},
        {"storage_file_too_large", QT_TRANSLATE_NOOP("BookOrbit", "The settings file is too large.")},
        {"storage_write_failed", QT_TRANSLATE_NOOP("BookOrbit", "Could not write file %1 (code %2).")},
        {"sync_complete", QT_TRANSLATE_NOOP("BookOrbit", "Sync complete")},
        {"sync_completed_with_issues", QT_TRANSLATE_NOOP("BookOrbit", "Sync completed with issues")},
        {"sync_stopped_by_user", QT_TRANSLATE_NOOP("BookOrbit", "Sync stopped by user")},
        {"sync_stopped_sign_in_again", QT_TRANSLATE_NOOP("BookOrbit", "Sync stopped: sign in again")},
        {"syncing_progress", QT_TRANSLATE_NOOP("BookOrbit", "Syncing progress…")},
        {"the_application_was_replaced_but_saving_was_not_confirmed_the_pr_5a3500", QT_TRANSLATE_NOOP("BookOrbit", "The application was replaced, but saving was not confirmed. The prepared update was kept; retry confirmation.")},
        {"the_application_was_replaced_confirm_saving_the_update_before_re_cdc870", QT_TRANSLATE_NOOP("BookOrbit", "The application was replaced. Confirm saving the update before removing the prepared file.")},
        {"the_book_file_has_changed_download_it_again", QT_TRANSLATE_NOOP("BookOrbit", "The book file has changed. Download it again.")},
        {"the_book_is_not_registered_in_the_library_yet_wait_and_tap_read_again", QT_TRANSLATE_NOOP("BookOrbit", "The book is not registered in the library yet. Wait and tap Read again.")},
        {"the_book_is_open_in_the_built_in_reader_close_it_and_retry_the_download", QT_TRANSLATE_NOOP("BookOrbit", "The book is open in the built-in reader. Close it and retry the download.")},
        {"the_download_folder_is_unavailable_choose_another_folder_in_settings", QT_TRANSLATE_NOOP("BookOrbit", "The download folder is unavailable. Choose another folder in Settings.")},
        {"the_file_is_unsupported_or_exceeds_100_mib", QT_TRANSLATE_NOOP("BookOrbit", "The file is unsupported or exceeds 100 MiB")},
        {"the_file_profile_or_account_changed_retry_the_operation", QT_TRANSLATE_NOOP("BookOrbit", "The file, profile or account changed. Retry the operation.")},
        {"the_incoming_position_has_not_been_applied_yet_sync_the_position_8547d4", QT_TRANSLATE_NOOP("BookOrbit", "The incoming position has not been applied yet. Sync the position or choose Read locally.")},
        {"the_local_book_file_changed_during_sync_progress_was_kept_unchanged", QT_TRANSLATE_NOOP("BookOrbit", "The local book file changed during sync. Progress was kept unchanged.")},
        {"the_local_file_matches_bookorbit", QT_TRANSLATE_NOOP("BookOrbit", "The local file matches BookOrbit")},
        {"the_local_position_format_is_unsupported_progress_was_kept_unchanged", QT_TRANSLATE_NOOP("BookOrbit", "The local position format is unsupported. Progress was kept unchanged.")},
        {"the_position_changed_while_you_were_choosing_review_the_choices_again", QT_TRANSLATE_NOOP("BookOrbit", "The position changed while you were choosing. Review the choices again.")},
        {"the_position_does_not_match_this_book", QT_TRANSLATE_NOOP("BookOrbit", "The position does not match this book.")},
        {"the_position_was_recognized_but_the_book_percentage_could_not_be_68fd96", QT_TRANSLATE_NOOP("BookOrbit", "The position was recognized, but the book percentage could not be estimated for upload. Positions were kept unchanged.")},
        {"the_position_was_recognized_but_the_book_percentage_could_not_be_b70578", QT_TRANSLATE_NOOP("BookOrbit", "The position was recognized, but the book percentage could not be estimated for the built-in library.")},
        {"the_published_release_is_unavailable", QT_TRANSLATE_NOOP("BookOrbit", "The published release is unavailable.")},
        {"the_reader_position_or_profile_has_changed_retry_sync", QT_TRANSLATE_NOOP("BookOrbit", "The reader position or profile has changed. Retry sync.")},
        {"the_reader_state_has_changed_retry_sync", QT_TRANSLATE_NOOP("BookOrbit", "The reader state has changed. Retry sync.")},
        {"the_release_information_is_invalid", QT_TRANSLATE_NOOP("BookOrbit", "The release information is invalid.")},
        {"the_saved_book_is_damaged_the_previous_book_is_still_available", QT_TRANSLATE_NOOP("BookOrbit", "The saved book is damaged. The previous book is still available.")},
        {"the_selected_file_is_no_longer_on_the_server_refresh_the_book_details", QT_TRANSLATE_NOOP("BookOrbit", "The selected file is no longer on the server. Refresh the book details.")},
        {"the_server_file_differs_progress_sync_is_paused", QT_TRANSLATE_NOOP("BookOrbit", "The server file differs. Progress sync is paused")},
        {"the_server_file_format_has_changed_refresh_the_book_details", QT_TRANSLATE_NOOP("BookOrbit", "The server file format has changed. Refresh the book details.")},
        {"the_server_has_no_exact_position_to_open", QT_TRANSLATE_NOOP("BookOrbit", "The server has no exact position to open.")},
        {"the_server_has_progress_without_an_exact_cfi_automatic_sync_is_n_df1162", QT_TRANSLATE_NOOP("BookOrbit", "The server has progress without an exact CFI. Automatic sync is not possible.")},
        {"the_server_is_unavailable_downloaded_books_can_be_read_offline", QT_TRANSLATE_NOOP("BookOrbit", "The server is unavailable. Downloaded books can be read offline.")},
        {"the_server_position_changed_after_upload_check_again", QT_TRANSLATE_NOOP("BookOrbit", "The server position changed after upload. Check again.")},
        {"the_server_position_includes_a_page_number_upload_stopped_to_preserve_it", QT_TRANSLATE_NOOP("BookOrbit", "The server position includes a page number. Upload stopped to preserve it.")},
        {"the_server_record_contains_other_reading_coordinates_upload_stop_23d8ab", QT_TRANSLATE_NOOP("BookOrbit", "The server record contains other reading coordinates. Upload stopped to preserve them.")},
        {"the_server_refused_the_connection_check_the_address_and_network", QT_TRANSLATE_NOOP("BookOrbit", "The server refused the connection. Check the address and network.")},
        {"the_server_response_contains_no_valid_session", QT_TRANSLATE_NOOP("BookOrbit", "The server response contains no valid session")},
        {"the_server_returned_an_invalid_catalog", QT_TRANSLATE_NOOP("BookOrbit", "The server returned an invalid catalog.")},
        {"the_server_returned_an_invalid_collection_list", QT_TRANSLATE_NOOP("BookOrbit", "The server returned an invalid collection list")},
        {"the_server_returned_invalid_book_details", QT_TRANSLATE_NOOP("BookOrbit", "The server returned invalid book details")},
        {"the_server_returned_invalid_data", QT_TRANSLATE_NOOP("BookOrbit", "The server returned invalid data")},
        {"the_update_archive_is_incomplete_or_damaged", QT_TRANSLATE_NOOP("BookOrbit", "The update archive is incomplete or damaged.")},
        {"the_update_connection_failed", QT_TRANSLATE_NOOP("BookOrbit", "The update connection failed.")},
        {"the_update_is_verified_press_install_and_close_to_apply_it", QT_TRANSLATE_NOOP("BookOrbit", "The update is verified. Press Install and close to apply it.")},
        {"the_update_package_could_not_be_verified", QT_TRANSLATE_NOOP("BookOrbit", "The update package could not be verified.")},
        {"the_update_request_timed_out", QT_TRANSLATE_NOOP("BookOrbit", "The update request timed out.")},
        {"the_update_response_is_too_large", QT_TRANSLATE_NOOP("BookOrbit", "The update response is too large.")},
        {"the_update_server_asked_to_wait_try_again_later", QT_TRANSLATE_NOOP("BookOrbit", "The update server asked to wait. Try again later.")},
        {"the_update_server_returned_an_unsafe_download_address", QT_TRANSLATE_NOOP("BookOrbit", "The update server returned an unsafe download address.")},
        {"this_fb2_structure_is_not_supported_for_sync_yet_images_tables_p_e1f2b9", QT_TRANSLATE_NOOP("BookOrbit", "This FB2 structure is not supported for sync yet. Images, tables, poetry and long sections require additional validation. You can still read the original file.")},
        {"this_file_is_already_linked_to_another_book_refresh_the_details", QT_TRANSLATE_NOOP("BookOrbit", "This file is already linked to another book. Refresh the details.")},
        {"update_download_cancelled", QT_TRANSLATE_NOOP("BookOrbit", "Update download cancelled.")},
        {"update_installed_but_temporary_files_could_not_be_removed_retry_cleanup", QT_TRANSLATE_NOOP("BookOrbit", "Update installed, but temporary files could not be removed. Retry cleanup.")},
        {"update_installed_open_bookorbit_again_from_the_applications_menu", QT_TRANSLATE_NOOP("BookOrbit", "Update installed. Open BookOrbit again from the applications menu.")},
        {"updates_are_unavailable_for_this_firmware", QT_TRANSLATE_NOOP("BookOrbit", "Updates are unavailable for this firmware.")},
        {"updates_can_only_be_installed_on_pocketbook_pb634", QT_TRANSLATE_NOOP("BookOrbit", "Updates can only be installed on PocketBook PB634.")},
        {"uploaded_but_the_result_could_not_be_saved_check_again", QT_TRANSLATE_NOOP("BookOrbit", "Uploaded, but the result could not be saved. Check again.")},
        {"verification_cancelled", QT_TRANSLATE_NOOP("BookOrbit", "Verification cancelled")},
        {"verification_complete", QT_TRANSLATE_NOOP("BookOrbit", "Verification complete")},
        {"verification_stopped_sign_in_again", QT_TRANSLATE_NOOP("BookOrbit", "Verification stopped: sign in again")},
        {"verify_server_http_error", QT_TRANSLATE_NOOP("BookOrbit", "Could not verify the server file (HTTP %1).")},
        {"verifying_the_update_package", QT_TRANSLATE_NOOP("BookOrbit", "Verifying the update package…")},
        {"wi_fi_is_disconnected_connect_to_a_network_and_try_again", QT_TRANSLATE_NOOP("BookOrbit", "Wi-Fi is disconnected. Connect to a network and try again.")},
        {"you_have_the_latest_version", QT_TRANSLATE_NOOP("BookOrbit", "You have the latest version.")},
    };
    return sources;
}

struct UiMessage {
    QString code;
    QJsonArray params;
    UiMessage() = default;
    explicit UiMessage(const char *key):code(QString::fromUtf8(key)) {}
    bool operator==(const UiMessage &) const = default;
    bool isEmpty() const { return code.isEmpty(); }
    void clear() { code.clear(); params={}; }
    UiMessage arg(const QJsonValue &value) const { auto result=*this; result.params.append(value); return result; }
    UiMessage arg(const UiMessage &value) const { return arg(QJsonValue(value.toJson())); }
    QJsonObject toJson() const { return {{"code",code},{"params",params}}; }
    static UiMessage fromLegacy(const QString &text);
    static UiMessage fromJson(const QJsonValue &value,int depth=0) {
        if(value.isString()) return fromLegacy(value.toString());
        if(value.isUndefined() || value.isNull()) return {};
        const auto object=value.toObject(); const auto key=object["code"].toString();
        if(object["code"].isString() && key.isEmpty() && object["params"].isArray() && object["params"].toArray().isEmpty()) return {};
        UiMessage fallback("previous_result_unavailable");
        if(depth>4 || !messageSources().contains(key) || !object["params"].isArray()) return fallback;
        const auto arguments=object["params"].toArray();
        static const QRegularExpression placeholder("%([1-9][0-9]*)");
        int count=0; auto matches=placeholder.globalMatch(QString::fromUtf8(messageSources()[key]));
        while(matches.hasNext()) count=qMax(count,matches.next().captured(1).toInt());
        if(count>9 || arguments.size()!=count) return fallback;
        if((key=="server_http_error" || key=="verify_server_http_error") &&
           (!arguments[0].isDouble() || arguments[0].toDouble()!=arguments[0].toInteger() ||
            arguments[0].toInteger()<100 || arguments[0].toInteger()>999)) return fallback;
        UiMessage result; result.code=key;
        for(const auto &argument:arguments) {
            if(argument.isString() && argument.toString().size()<=4096) result.params.append(argument);
            else if(argument.isDouble() && std::isfinite(argument.toDouble())) result.params.append(argument);
            else if(argument.isObject()) result.params.append(fromJson(argument,depth+1).toJson());
            else return fallback;
        }
        return result;
    }
    QString text() const {
        if(isEmpty()) return {};
        const auto valid=fromJson(toJson());
        if(valid!=*this) return valid.text();
        const auto source=messageSources().value(code);
        const auto translated=QCoreApplication::translate("BookOrbit",source);
        static const QRegularExpression placeholder("%([1-9][0-9]*)");
        auto matches=placeholder.globalMatch(translated); QString result; qsizetype offset=0;
        while(matches.hasNext()) {
            const auto match=matches.next(); const int index=match.captured(1).toInt()-1;
            result+=translated.mid(offset,match.capturedStart()-offset);
            const auto value=params.at(index);
            result+=value.isObject() ? fromJson(value).text() : value.isString() ? value.toString() : QString::number(value.toDouble(),'g',15);
            offset=match.capturedEnd();
        }
        return result+translated.mid(offset);
    }
};
inline UiMessage uiMessage(const char *code) { return UiMessage(code); }
inline UiMessage messageForSource(const char *source) {
    for(auto it=messageSources().cbegin();it!=messageSources().cend();++it)
        if(QString::fromUtf8(it.value())==QString::fromUtf8(source)) { UiMessage message; message.code=it.key(); return message; }
    return uiMessage("previous_result_unavailable");
}
inline UiMessage UiMessage::fromLegacy(const QString &text) {
    if(text.isEmpty()) return {};
    if(text.size()>16384) return uiMessage("previous_result_unavailable");
    static QTranslator russian;
    static const bool loaded=russian.load(":/translations/bookorbit_ru.qm");
    for(auto it=messageSources().cbegin();it!=messageSources().cend();++it) {
        if(text==QString::fromUtf8(it.value()) || (loaded && text==russian.translate("BookOrbit",it.value()))) {
            UiMessage message; message.code=it.key(); return message;
        }
    }
    for(const auto *key:{"server_http_error","verify_server_http_error","server_not_found_1_check_wi_fi_and_dns"}) {
        const auto source=messageSources().value(key);
        for(const auto &pattern:{QString::fromUtf8(source),loaded ? russian.translate("BookOrbit",source) : QString{}}) {
            const auto parts=pattern.split("%1"); if(parts.size()!=2) continue;
            const bool http=QString::fromUtf8(key).contains("http");
            const auto regex="\\A"+QRegularExpression::escape(parts[0])+(http ? "([1-9][0-9]{2})" : "(.{1,4096}?)")+QRegularExpression::escape(parts[1])+"\\z";
            const auto match=QRegularExpression(regex).match(text);
            if(match.hasMatch()) return http ? uiMessage(key).arg(match.captured(1).toInt()) : uiMessage(key).arg(match.captured(1));
        }
    }
    return uiMessage("previous_result_unavailable");
}
inline QString translatedText(const QString &legacy) { return UiMessage::fromLegacy(legacy).text(); }


class InterfaceTranslation {
public:
    void apply(const QString &systemLanguage, const QString &preference) {
        QCoreApplication::removeTranslator(&translator);
        const QString language = interfaceLanguage(systemLanguage, preference);
        if (language != "en") {
            if (!translator.load(":/translations/bookorbit_"+language+".qm")) qFatal("Missing translation catalog");
            QCoreApplication::installTranslator(&translator);
        }
        QLocale::setDefault(QLocale(language));
    }
private:
    QTranslator translator;
};
