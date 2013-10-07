// Copyright 2013 Tanel Lebedev

#include "./toggl_api_client.h"

#include <string>
#include <cstring>
#include <sstream>

#include "Poco/Stopwatch.h"
#include "Poco/Exception.h"
#include "Poco/Logger.h"
#include "Poco/DateTimeParser.h"
#include "Poco/DateTime.h"
#include "Poco/DateTimeFormatter.h"
#include "Poco/DateTimeFormat.h"
#include "Poco/Timespan.h"
#include "Poco/NumberParser.h"

#include "./https_client.h"
#include "./version.h"

#include "./libjson.h"

namespace kopsik {

const char *known_colors[] = {
    "#4dc3ff", "#bc85e6", "#df7baa", "#f68d38", "#b27636",
    "#8ab734", "#14a88e", "#268bb5", "#6668b4", "#a4506c",
    "#67412c", "#3c6526", "#094558", "#bc2d07", "#999999"
};
template<typename T, size_t N> T *end(T (&ra)[N]) {
    return ra + N;
}
std::vector<std::string> Project::color_codes(known_colors, end(known_colors));

// Start a time entry, mark it as dirty and add to user time entry collection.
// Do not save here, dirtyness will be handled outside of this module.
TimeEntry *User::Start(std::string description) {
  Stop();
  TimeEntry *te = new TimeEntry();
  te->SetDescription(description);
  te->SetUID(ID());
  te->SetStart(time(0));
  te->SetDurationInSeconds(-time(0));
  te->SetWID(DefaultWID());
  te->SetUIModifiedAt(time(0));
  te->SetCreatedWith(createdWith());
  related.TimeEntries.push_back(te);
  return te;
}

TimeEntry *User::Continue(std::string GUID) {
    Stop();
    TimeEntry *existing = GetTimeEntryByGUID(GUID);
    poco_assert(existing);
    TimeEntry *te = new TimeEntry();
    te->SetDescription(existing->Description());
    te->SetUID(ID());
    te->SetStart(time(0));
    te->SetDurationInSeconds(-time(0));
    te->SetWID(existing->WID());
    te->SetPID(existing->PID());
    te->SetTID(existing->TID());
    te->SetUIModifiedAt(time(0));
    te->SetCreatedWith(createdWith());
    related.TimeEntries.push_back(te);
    return te;
}

void User::DeleteTimeEntry(std::string GUID) {
    TimeEntry *te = GetTimeEntryByGUID(GUID);
    poco_assert(te);
    te->SetDeletedAt(time(0));
    te->SetUIModifiedAt(time(0));
}

std::string User::createdWith() {
    std::stringstream ss;
    ss  << "libkopsik/"
        << kopsik::version::Major
        << "."
        << kopsik::version::Minor
        << "."
        << kopsik::version::Patch;
    return ss.str();
}

error User::ListenToWebsocket(HTTPSClient *https_client) {
  return https_client->ListenToWebsocket();
}

bool compareTimeEntriesByStart(TimeEntry *a, TimeEntry *b) {
  return a->Start() > b->Start();
}

void User::SortTimeEntriesByStart() {
  std::sort(related.TimeEntries.begin(), related.TimeEntries.end(),
    compareTimeEntriesByStart);
}

void User::SetFullname(std::string value) {
  if (fullname_ != value) {
    fullname_ = value;
    dirty_ = true;
  }
}

void User::SetAPIToken(std::string value) {
    if (api_token_ != value) {
        api_token_ = value;
        dirty_ = true;
    }
}

void User::SetSince(Poco::UInt64 value) {
    if (since_ != value) {
        since_ = value;
        dirty_ = true;
    }
}

void User::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void User::SetDefaultWID(Poco::UInt64 value) {
    if (default_wid_ != value) {
        default_wid_ = value;
        dirty_ = true;
    }
}

// Stop a time entry, mark it as dirty.
// Note that there may be multiple TE-s running. If there are,
// all of them are stopped (multi-tracking is not supported by Toggl).
// Do not save here, dirtyness will be handled outside of this module.
std::vector<TimeEntry *> User::Stop() {
    std::vector<TimeEntry *> result;
    TimeEntry *te = RunningTimeEntry();
    while (te) {
        result.push_back(te);
        te->SetDurationInSeconds(time(0) + te->DurationInSeconds());
        te->SetUIModifiedAt(time(0));
        te = RunningTimeEntry();
    }
    return result;
}

TimeEntry *User::RunningTimeEntry() {
    for (std::vector<TimeEntry *>::const_iterator it =
            related.TimeEntries.begin();
            it != related.TimeEntries.end();
            it++) {
        if ((*it)->DurationInSeconds() < 0) {
            return *it;
        }
    }
    return 0;
}

bool TimeEntry::NeedsPush() {
    return NeedsPOST() || NeedsPUT() || NeedsDELETE();
}

bool TimeEntry::NeedsPOST() {
    // No server side ID yet, meaning it's not POSTed yet
    return !id_;
}

bool TimeEntry::NeedsPUT() {
    // User has modified model via UI, needs a PUT
    return ui_modified_at_ > 0;
}

bool TimeEntry::NeedsDELETE() {
    // TE is deleted, needs a DELETE
    return deleted_at_ > 0;
}

void User::CollectPushableObjects(std::vector<TimeEntry *> *result) {
    poco_assert(result);
    for (std::vector<TimeEntry *>::const_iterator it =
            related.TimeEntries.begin();
            it != related.TimeEntries.end();
            it++) {
        TimeEntry *te = *it;
        if (te->NeedsPush()) {
            result->push_back(te);
        }
    }
}

error User::Push(HTTPSClient *https_client) {
    Poco::Stopwatch stopwatch;
    stopwatch.start();

    std::vector<TimeEntry *>dirty;
    CollectPushableObjects(&dirty);

    Poco::Logger &logger = Poco::Logger::get("toggl_api_client");
    if (dirty.empty()) {
        logger.debug("Nothing to push.");
        return noError;
    }

    {
        std::stringstream ss;
        ss << dirty.size() << " model(s) need a push";
        logger.debug(ss.str());
    }

    // Convert the dirty objects to batch updates JSON.
    JSONNODE *c = json_new(JSON_ARRAY);
    for (std::vector<TimeEntry *>::const_iterator it =
            dirty.begin();
            it != dirty.end(); it++) {
        TimeEntry *te = *it;
        JSONNODE *n = te->JSON();
        json_set_name(n, "time_entry");

        JSONNODE *body = json_new(JSON_NODE);
        json_set_name(body, "body");
        json_push_back(body, n);

        JSONNODE *update = json_new(JSON_NODE);
        if (te->NeedsPOST()) {
            json_push_back(update, json_new_a("method", "POST"));
            json_push_back(update, json_new_a("relative_url",
                "/api/v8/time_entries"));
            std::stringstream ss;
            ss << "Time entry " << te->String() << " needs a POST";
            logger.debug(ss.str());

        } else if (te->NeedsPUT()) {
            std::stringstream url;
            url << "/api/v8/time_entries/" << te->ID();
            json_push_back(update, json_new_a("method", "PUT"));
            json_push_back(update, json_new_a("relative_url",
                url.str().c_str()));
            std::stringstream ss;
            ss << "Time entry " << te->String() << " needs a PUT";
            logger.debug(ss.str());

        } else if (te->NeedsDELETE()) {
            std::stringstream url;
            url << "/api/v8/time_entries/" << te->ID();
            json_push_back(update, json_new_a("method", "DELETE"));
            json_push_back(update, json_new_a("relative_url",
                url.str().c_str()));
            std::stringstream ss;
            ss << "Time entry " << te->String() << " needs a DELETE";
            logger.debug(ss.str());
        }
        json_push_back(update, json_new_a("GUID", te->GUID().c_str()));
        json_push_back(update, body);

        json_push_back(c, update);
    }
    json_char *jc = json_write_formatted(c);
    std::string json(jc);
    json_free(jc);
    json_delete(c);

    logger.debug(json);

    std::string response_body("");
    error err = https_client->PostJSON("/api/v8/batch_updates",
        json,
        APIToken(),
        "api_token",
        &response_body);
    if (err != noError) {
        return err;
    }

    std::vector<BatchUpdateResult> results;
    parseResponseArray(response_body, &results);

    // Iterate through response array, parse response bodies.
    // Collect errors into a vector.
    std::vector<error> errors;
    for (std::vector<BatchUpdateResult>::const_iterator it = results.begin();
            it != results.end();
            it++) {
        BatchUpdateResult result = *it;

        std::stringstream ss;
        ss  << "batch update result GUID: " << result.GUID
            << ", StatusCode: " << result.StatusCode
            << ", ContentType: " << result.ContentType
            << ", Body: " << result.Body;
        logger.debug(ss.str());

        if (result.StatusCode < 200 || result.StatusCode >= 300) {
            ss  << "ERROR! update result GUID: " << result.GUID
                << ", StatusCode: " << result.StatusCode
                << ", ContentType: " << result.ContentType
                << ", Body: " << result.Body;
            logger.error(ss.str());
            errors.push_back(result.Body);
        }

        poco_assert(!result.GUID.empty());
        poco_assert(json_is_valid(result.Body.c_str()));

        TimeEntry *te = 0;
        for (std::vector<TimeEntry *>::const_iterator it =
                dirty.begin(); it != dirty.end(); it++) {
            if ((*it)->GUID() == result.GUID) {
                te = *it;
                break;
            }
        }
        poco_assert(te);

        JSONNODE *n = json_parse(result.Body.c_str());
        JSONNODE_ITERATOR i = json_begin(n);
        JSONNODE_ITERATOR e = json_end(n);
        while (i != e) {
            json_char *node_name = json_name(*i);
            if (strcmp(node_name, "data") == 0) {
                te->LoadFromJSONNode(*i);
            }
            ++i;
        }
        json_delete(n);
    }

    // Collect errors
    if (!errors.empty()) {
        for (std::vector<error>::const_iterator it = errors.begin();
                it != errors.end();
                it++) {
            error err = *it;
            logger.error(err);
        }

        std::stringstream ss;
        std::copy(errors.begin(), errors.end(),
            std::ostream_iterator<std::string>(ss, ". "));
        return error(ss.str());
    }

    stopwatch.stop();
    std::stringstream ss;
    ss << "Changes data JSON pushed and responses parsed in "
        << stopwatch.elapsed() / 1000 << " ms";
    logger.debug(ss.str());

    return noError;
}

void User::parseResponseArray(std::string response_body,
        std::vector<BatchUpdateResult> *responses) {
    poco_assert(responses);
    poco_assert(!response_body.empty());
    JSONNODE *response_array = json_parse(response_body.c_str());
    JSONNODE_ITERATOR i = json_begin(response_array);
    JSONNODE_ITERATOR e = json_end(response_array);
    while (i != e) {
        BatchUpdateResult result;
        result.parseResponseJSON(*i);
        responses->push_back(result);
        ++i;
    }
    json_delete(response_array);
}

void BatchUpdateResult::parseResponseJSON(JSONNODE *n) {
    poco_assert(n);
    StatusCode = 0;
    Body = "";
    GUID = "";
    ContentType = "";
    JSONNODE_ITERATOR i = json_begin(n);
    JSONNODE_ITERATOR e = json_end(n);
    while (i != e) {
        json_char *node_name = json_name(*i);
        if (strcmp(node_name, "status") == 0) {
            StatusCode = json_as_int(*i);
        } else if (strcmp(node_name, "body") == 0) {
            Body = std::string(json_as_string(*i));
        } else if (strcmp(node_name, "guid") == 0) {
            GUID = std::string(json_as_string(*i));
        } else if (strcmp(node_name, "guid") == 0) {
            ContentType = std::string(json_as_string(*i));
        }
        ++i;
    }
}

error User::Login(HTTPSClient *https_client,
    const std::string &email, const std::string &password) {
  LoginEmail = email;
  LoginPassword = password;
  return pull(https_client, false, true);
}

error User::Sync(HTTPSClient *https_client, bool full_sync) {
    error err = pull(https_client, true, full_sync);
    if (err != noError) {
        return err;
    }
    return Push(https_client);
}

error User::pull(HTTPSClient *https_client,
      bool authenticate_with_api_token, bool full_sync) {
  Poco::Stopwatch stopwatch;
  stopwatch.start();

  std::stringstream relative_url;
  relative_url << "/api/v8/me?with_related_data=true";
  if (!full_sync) {
      relative_url << "&since=" << since_;
  }

  std::string basic_auth_username("");
  std::string basic_auth_password("");
  if (authenticate_with_api_token) {
    basic_auth_username = APIToken();
    basic_auth_password = "api_token";
  } else {
    basic_auth_username = LoginEmail;
    basic_auth_password = LoginPassword;
  }
  std::string response_body("");

  error err = https_client->GetJSON(relative_url.str(),
    basic_auth_username,
    basic_auth_password,
    &response_body);
  if (err != noError) {
    return err;
  }

  LoadFromJSONString(response_body, true);

  stopwatch.stop();
  std::stringstream ss;
  ss << "User with related data JSON fetched and parsed in "
    << stopwatch.elapsed() / 1000 << " ms";
  Poco::Logger &logger = Poco::Logger::get("toggl_api_client");
  logger.debug(ss.str());

  return noError;
};

void User::LoadFromJSONString(const std::string &json,
        bool with_related_data) {
    poco_assert(!json.empty());

    Poco::Stopwatch stopwatch;
    stopwatch.start();

    Poco::Logger &logger = Poco::Logger::get("toggl_api_client");

    JSONNODE *root = json_parse(json.c_str());
    JSONNODE_ITERATOR current_node = json_begin(root);
    JSONNODE_ITERATOR last_node = json_end(root);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "since") == 0) {
            SetSince(json_as_int(*current_node));
            std::stringstream s;
            s << "User data as of: " << Since();
            logger.debug(s.str());
        } else if (strcmp(node_name, "data") == 0) {
            LoadDataFromJSONNode(*current_node, with_related_data);
        }
        ++current_node;
    }
    json_delete(root);

    stopwatch.stop();
    std::stringstream ss;
    ss << json.length() << " bytes of JSON parsed in " <<
        stopwatch.elapsed() / 1000 << " ms";
    logger.debug(ss.str());
}

std::string User::String() {
    std::stringstream ss;
    ss << "ID=" << id_ <<
        " default_wid=" << default_wid_ <<
        " local_id=" << local_id_ <<
        " api_token=" << api_token_ <<
        " since=" << since_;
    return ss.str();
}

void User::LoadDataFromJSONNode(JSONNODE *data, bool with_related_data) {
    poco_assert(data);

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "default_wid") == 0) {
            SetDefaultWID(json_as_int(*current_node));
        } else if (strcmp(node_name, "api_token") == 0) {
            SetAPIToken(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "fullname") == 0) {
            SetFullname(std::string(json_as_string(*current_node)));
        } else if (with_related_data) {
            if (strcmp(node_name, "projects") == 0) {
                loadProjectsFromJSONNode(*current_node);
            } else if (strcmp(node_name, "tags") == 0) {
                loadTagsFromJSONNode(*current_node);
            } else if (strcmp(node_name, "tasks") == 0) {
                loadTasksFromJSONNode(*current_node);
            } else if (strcmp(node_name, "time_entries") == 0) {
                loadTimeEntriesFromJSONNode(*current_node);
            } else if (strcmp(node_name, "workspaces") == 0) {
                loadWorkspacesFromJSONNode(*current_node);
            } else if (strcmp(node_name, "clients") == 0) {
                loadClientsFromJSONNode(*current_node);
            }
        }
        ++current_node;
    }
}

void User::loadProjectsFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        Project *model = GetProjectByID(id);
        if (!model) {
            model = new Project();
            related.Projects.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string Project::String() {
    std::stringstream ss;
    ss << "ID=" << id_ << " name=" << name_ << " wid=" << wid_ <<
        " guid=" << guid_;
    return ss.str();
}

void Project::SetName(std::string value) {
    if (name_ != value) {
        name_ = value;
        dirty_ = true;
    }
}

void Project::SetColor(std::string value) {
    if (color_ != value) {
        color_ = value;
        dirty_ = true;
    }
}

std::string Project::ColorCode() {
    int index(0);
    if (!Poco::NumberParser::tryParse(Color(), index)) {
        return color_codes.back();
    }
    if (!index) {
        return color_codes.back();
    }
    return color_codes[index % color_codes.size()];
}

void Project::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void Project::SetGUID(std::string value) {
    if (guid_ != value) {
        guid_ = value;
        dirty_ = true;
    }
}

void Project::SetWID(Poco::UInt64 value) {
    if (wid_ != value) {
        wid_ = value;
        dirty_ = true;
    }
}

void Project::SetCID(Poco::UInt64 value) {
    if (cid_ != value) {
        cid_ = value;
        dirty_ = true;
    }
}

void Project::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void User::loadTasksFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        Task *model = GetTaskByID(id);
        if (!model) {
            model = new Task();
            related.Tasks.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string Task::String() {
    std::stringstream ss;
    ss << "ID=" << id_ << " name=" << name_ << " wid=" << wid_ <<
        " pid=" << pid_;
    return ss.str();
}

void Task::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void Task::SetPID(Poco::UInt64 value) {
    if (pid_ != value) {
        pid_ = value;
        dirty_ = true;
    }
}

void Task::SetWID(Poco::UInt64 value) {
    if (wid_ != value) {
        wid_ = value;
        dirty_ = true;
    }
}

void Task::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void Task::SetName(std::string value) {
    if (name_ != value) {
        name_ = value;
        dirty_ = true;
    }
}

void User::loadWorkspacesFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        Workspace *model = GetWorkspaceByID(id);
        if (!model) {
            model = new Workspace();
            related.Workspaces.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string Workspace::String() {
    std::stringstream ss;
    ss << "ID=" << id_ << " name=" << name_;
    return ss.str();
}

void Workspace::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void Workspace::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void Workspace::SetName(std::string value) {
    if (name_ != value) {
        name_ = value;
        dirty_ = true;
    }
}

void User::loadTagsFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        Tag *model = GetTagByID(id);
        if (!model) {
            model = new Tag();
            related.Tags.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string Tag::String() {
    std::stringstream ss;
    ss << "ID=" << id_ << " name=" << name_ << " wid=" << wid_ <<
        " guid=" << guid_;
    return ss.str();
}

void Tag::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void Tag::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void Tag::SetWID(Poco::UInt64 value) {
    if (wid_ != value) {
        wid_ = value;
        dirty_ = true;
    }
}

void Tag::SetName(std::string value) {
    if (name_ != value) {
        name_ = value;
        dirty_ = true;
    }
}

void Tag::SetGUID(std::string value) {
    if (guid_ != value) {
        guid_ = value;
        dirty_ = true;
    }
}

void User::loadClientsFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        Client *model = GetClientByID(id);
        if (!model) {
            model = new Client();
            related.Clients.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string Client::String() {
    std::stringstream ss;
    ss << "ID=" << id_ << " name=" << name_ << " wid=" << wid_ <<
        " guid=" << guid_;
    return ss.str();
}

void Client::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void Client::SetName(std::string value) {
    if (name_ != value) {
        name_ = value;
        dirty_ = true;
    }
}

void Client::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void Client::SetGUID(std::string value) {
    if (guid_ != value) {
        guid_ = value;
        dirty_ = true;
    }
}

void Client::SetWID(Poco::UInt64 value) {
    if (wid_ != value) {
        wid_ = value;
        dirty_ = true;
    }
}

Poco::UInt64 User::getIDFromJSONNode(JSONNODE *data) {
    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            return json_as_int(*current_node);
        }
        ++current_node;
    }
    poco_assert(false);
    return 0;
}

void User::loadTimeEntriesFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        Poco::UInt64 id = getIDFromJSONNode(*current_node);
        TimeEntry *model = GetTimeEntryByID(id);
        if (!model) {
            model = new TimeEntry();
            related.TimeEntries.push_back(model);
        }
        model->SetUID(ID());
        model->LoadFromJSONNode(*current_node);
        ++current_node;
    }
}

std::string TimeEntry::String() {
    std::stringstream ss;
    ss  << "ID=" << id_
        << " description=" << description_
        << " wid=" << wid_
        << " guid=" << guid_
        << " pid=" << pid_
        << " tid=" << tid_
        << " start=" << start_
        << " stop=" << stop_
        << " duration=" << duration_in_seconds_
        << " billable=" << billable_
        << " duronly=" << duronly_
        << " tags=" << Tags()
        << " created_with=" << CreatedWith()
        << " ui_modified_at=" << ui_modified_at_;
    return ss.str();
}

JSONNODE *TimeEntry::JSON() {
    JSONNODE *n = json_new(JSON_NODE);
    json_set_name(n, "time_entry");
    if (id_) {
        json_push_back(n, json_new_i("id", (json_int_t)id_));
    }
    json_push_back(n, json_new_a("description", description_.c_str()));
    json_push_back(n, json_new_i("wid", (json_int_t)wid_));
    json_push_back(n, json_new_a("guid", guid_.c_str()));
    if (pid_) {
        json_push_back(n, json_new_i("pid", (json_int_t)pid_));
    }
    if (tid_) {
        json_push_back(n, json_new_i("tid", (json_int_t)tid_));
    }
    json_push_back(n, json_new_a("start", StartString().c_str()));
    if (stop_) {
        json_push_back(n, json_new_a("stop", StopString().c_str()));
    }
    json_push_back(n, json_new_i("duration", (json_int_t)duration_in_seconds_));
    json_push_back(n, json_new_b("billable", billable_));
    json_push_back(n, json_new_b("duronly", duronly_));
    json_push_back(n, json_new_i("ui_modified_at",
        (json_int_t)ui_modified_at_));
    json_push_back(n, json_new_a("created_with", created_with_.c_str()));

    return n;
}

Workspace *User::GetWorkspaceByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<Workspace *>::const_iterator it =
            related.Workspaces.begin();
            it != related.Workspaces.end();
            it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

Client *User::GetClientByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<Client *>::const_iterator it = related.Clients.begin();
            it != related.Clients.end(); it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

Project *User::GetProjectByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<Project *>::const_iterator it = related.Projects.begin();
            it != related.Projects.end(); it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

Task *User::GetTaskByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<Task *>::const_iterator it = related.Tasks.begin();
            it != related.Tasks.end(); it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

Tag *User::GetTagByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<Tag *>::const_iterator it = related.Tags.begin();
            it != related.Tags.end(); it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

TimeEntry *User::GetTimeEntryByID(const Poco::UInt64 id) {
    poco_assert(id > 0);
    for (std::vector<TimeEntry *>::const_iterator it =
            related.TimeEntries.begin();
            it != related.TimeEntries.end();
            it++) {
        if ((*it)->ID() == id) {
            return *it;
        }
    }
    return 0;
}

TimeEntry *User::GetTimeEntryByGUID(std::string GUID) {
    poco_assert(!GUID.empty());
    for (std::vector<TimeEntry *>::const_iterator it =
            related.TimeEntries.begin();
            it != related.TimeEntries.end();
            it++) {
        if ((*it)->GUID() == GUID) {
            return *it;
        }
    }
    return 0;
}

void User::ClearWorkspaces() {
    for (std::vector<Workspace *>::const_iterator it =
            related.Workspaces.begin();
            it != related.Workspaces.end();
            it++) {
        delete *it;
    }
    related.Workspaces.clear();
}

void User::ClearProjects() {
    for (std::vector<Project *>::const_iterator it =
            related.Projects.begin();
            it != related.Projects.end();
            it++) {
        delete *it;
    }
    related.Projects.clear();
}

void User::ClearTasks() {
    for (std::vector<Task *>::const_iterator it =
            related.Tasks.begin();
            it != related.Tasks.end();
            it++) {
        delete *it;
    }
    related.Tasks.clear();
}

void User::ClearTags() {
    for (std::vector<Tag *>::const_iterator it =
            related.Tags.begin();
            it != related.Tags.end();
            it++) {
        delete *it;
    }
    related.Tags.clear();
}

void User::ClearClients() {
    for (std::vector<Client *>::const_iterator it =
            related.Clients.begin();
            it != related.Clients.end();
            it++) {
        delete *it;
    }
    related.Clients.clear();
}

void User::ClearTimeEntries() {
    for (std::vector<TimeEntry *>::const_iterator it =
            related.TimeEntries.begin();
            it != related.TimeEntries.end();
            it++) {
        delete *it;
    }
    related.TimeEntries.clear();
}

void Workspace::LoadFromJSONNode(JSONNODE *n) {
    poco_assert(n);
    JSONNODE_ITERATOR i = json_begin(n);
    JSONNODE_ITERATOR e = json_end(n);
    while (i != e) {
        json_char *node_name = json_name(*i);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*i));
        } else if (strcmp(node_name, "name") == 0) {
            SetName(std::string(json_as_string(*i)));
        }
        ++i;
    }
}

void Client::LoadFromJSONNode(JSONNODE *data) {
    poco_assert(data);

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "name") == 0) {
            SetName(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "guid") == 0) {
            SetGUID(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "wid") == 0) {
            SetWID(json_as_int(*current_node));
        }
        ++current_node;
    }
}

void Project::LoadFromJSONNode(JSONNODE *data) {
    poco_assert(data);

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "name") == 0) {
            SetName(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "guid") == 0) {
            SetGUID(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "wid") == 0) {
            SetWID(json_as_int(*current_node));
        } else if (strcmp(node_name, "cid") == 0) {
            SetCID(json_as_int(*current_node));
        } else if (strcmp(node_name, "color") == 0) {
            SetColor(std::string(json_as_string(*current_node)));
        }
        ++current_node;
    }
}

void Task::LoadFromJSONNode(JSONNODE *data) {
    poco_assert(data);

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "name") == 0) {
            SetName(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "pid") == 0) {
            SetPID(json_as_int(*current_node));
        } else if (strcmp(node_name, "wid") == 0) {
            SetWID(json_as_int(*current_node));
        }
        ++current_node;
    }
}

void Tag::LoadFromJSONNode(JSONNODE *data) {
    poco_assert(data);

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "name") == 0) {
            SetName(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "guid") == 0) {
            SetGUID(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "wid") == 0) {
            SetWID(json_as_int(*current_node));
        }
        ++current_node;
    }
}

std::time_t TimeEntry::Parse8601(std::string iso_8601_formatted_date) {
    int tzd;
    Poco::DateTime dt;
    Poco::DateTimeParser::parse(Poco::DateTimeFormat::ISO8601_FORMAT,
        iso_8601_formatted_date, dt, tzd);
    Poco::Timestamp ts = dt.timestamp();
    return ts.epochTime();
}

std::string TimeEntry::Format8601(std::time_t date) {
    Poco::Timestamp ts = Poco::Timestamp::fromEpochTime(date);
    return Poco::DateTimeFormatter::format(ts,
        Poco::DateTimeFormat::ISO8601_FORMAT);
}

std::string TimeEntry::StartString() {
     return Format8601(start_);
}

void TimeEntry::SetStartString(std::string value) {
    SetStart(Parse8601(value));
}

// FIXME: add tests for this
std::string TimeEntry::DurationString() {
    return Formatter::FormatDurationInSeconds(duration_in_seconds_);
}

void TimeEntry::SetDurationString(std::string value) {
    // FIXME: parse duration string into duration in seconds
}

void TimeEntry::SetCreatedWith(std::string value) {
    if (created_with_ != value) {
        created_with_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetDurOnly(bool value) {
    if (duronly_ != value) {
        duronly_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetBillable(bool value) {
    if (billable_ != value) {
        billable_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetDurationInSeconds(Poco::Int64 value) {
    if (duration_in_seconds_ != value) {
        duration_in_seconds_ = value;
        dirty_ = true;
    }
}

std::string TimeEntry::StopString() {
     return Format8601(stop_);
}

void TimeEntry::SetStopString(std::string value) {
    SetStop(Parse8601(value));
}

void TimeEntry::SetStop(Poco::UInt64 value) {
    if (stop_ != value) {
        stop_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetDeletedAt(Poco::UInt64 value) {
    if (deleted_at_ != value) {
        deleted_at_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetStart(Poco::UInt64 value) {
    if (start_ != value) {
        start_ = value;
        dirty_ = true;
    }
}

std::string TimeEntry::Tags() {
    std::stringstream ss;
    std::copy(TagNames.begin(), TagNames.end(),
        std::ostream_iterator<std::string>(ss, "|"));
    return ss.str();
}

void TimeEntry::SetTags(std::string tags) {
    if (Tags() != tags) {
        TagNames.clear();
        std::stringstream ss(tags);
        while (ss.good()) {
            std::string tag;
            getline(ss, tag, '|');
            TagNames.push_back(tag);
        }
        dirty_ = true;
    }
}

Poco::UInt64 TimeEntry::getUIModifiedAtFromJSONNode(JSONNODE *data) {
    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "ui_modified_at") == 0) {
            return json_as_int(*current_node);
        }
        ++current_node;
    }
    return 0;
}

void TimeEntry::LoadFromJSONString(std::string json) {
    poco_assert(!json.empty());
    JSONNODE *root = json_parse(json.c_str());
    LoadFromJSONNode(root);
    json_delete(root);
}

void TimeEntry::LoadFromJSONNode(JSONNODE *data) {
    poco_assert(data);

    Poco::UInt64 ui_modified_at =
        getUIModifiedAtFromJSONNode(data);
    if (UIModifiedAt() > ui_modified_at) {
        Poco::Logger &logger = Poco::Logger::get("toggl_api_client");
        std::stringstream ss;
        ss  << "Will not overwrite time entry "
            << String()
            << " with server data because we have a ui_modified_at";
        logger.debug(ss.str());
        return;
    }

    JSONNODE_ITERATOR current_node = json_begin(data);
    JSONNODE_ITERATOR last_node = json_end(data);
    while (current_node != last_node) {
        json_char *node_name = json_name(*current_node);
        if (strcmp(node_name, "id") == 0) {
            SetID(json_as_int(*current_node));
        } else if (strcmp(node_name, "description") == 0) {
            SetDescription(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "guid") == 0) {
            SetGUID(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "wid") == 0) {
            SetWID(json_as_int(*current_node));
        } else if (strcmp(node_name, "pid") == 0) {
            SetPID(json_as_int(*current_node));
        } else if (strcmp(node_name, "tid") == 0) {
            SetTID(json_as_int(*current_node));
        } else if (strcmp(node_name, "start") == 0) {
            SetStartString(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "stop") == 0) {
            SetStopString(std::string(json_as_string(*current_node)));
        } else if (strcmp(node_name, "duration") == 0) {
            SetDurationInSeconds(json_as_int(*current_node));
        } else if (strcmp(node_name, "billable") == 0) {
            SetBillable(json_as_bool(*current_node));
        } else if (strcmp(node_name, "duronly") == 0) {
            SetDurOnly(json_as_bool(*current_node));
        } else if (strcmp(node_name, "tags") == 0) {
            loadTagsFromJSONNode(*current_node);
        } else if (strcmp(node_name, "created_with") == 0) {
            SetCreatedWith(std::string(json_as_string(*current_node)));
        }
        ++current_node;
    }

    SetUIModifiedAt(0);
}

void TimeEntry::SetWID(Poco::UInt64 value) {
    if (wid_ != value) {
        wid_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetID(Poco::UInt64 value) {
    if (id_ != value) {
        id_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetPID(Poco::UInt64 value) {
    if (pid_ != value) {
        pid_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetTID(Poco::UInt64 value) {
    if (tid_ != value) {
        tid_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetUID(Poco::UInt64 value) {
    if (uid_ != value) {
        uid_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetDescription(std::string value) {
    if (description_ != value) {
        description_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetGUID(std::string value) {
    if (guid_ != value) {
        guid_ = value;
        dirty_ = true;
    }
}

void TimeEntry::SetUIModifiedAt(Poco::UInt64 value) {
    if (ui_modified_at_ != value) {
        ui_modified_at_ = value;
        dirty_ = true;
    }
}

error TimeEntry::loadTagsFromJSONNode(JSONNODE *list) {
    poco_assert(list);

    TagNames.clear();

    JSONNODE_ITERATOR current_node = json_begin(list);
    JSONNODE_ITERATOR last_node = json_end(list);
    while (current_node != last_node) {
        std::string tag = std::string(json_as_string(*current_node));
        if (!tag.empty()) {
            TagNames.push_back(tag);
        }
        ++current_node;
    }
    return noError;
}

std::string Formatter::FormatDurationInSeconds(Poco::Int64 value) {
    if (value < 0) {
        value = time(0) + value;
    }
    Poco::Timespan span(value * Poco::Timespan::SECONDS);
    if (span.totalHours() > 0) {
        return Poco::DateTimeFormatter::format(span, "%H:%M:%S");
    }
    if (span.totalMinutes() > 0) {
        return Poco::DateTimeFormatter::format(span, "%M:%S min");
    }
    std::ostringstream out;
    out << span.totalSeconds() << " sec";
    return out.str();
}

}   // namespace kopsik
