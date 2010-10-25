//  Copyright (c) 2010 David Caldwell,  All Rights Reserved.

#include "daemon.h"
#include "permissions.h"
#include "config.h"
#include "passwd.h"
#include "strprintf.h"
#include "log.h"
#include "key-exists.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <algorithm>
#include <libgen.h>

using namespace std;

daemon::daemon(string config_file, class user *user)
        : config_file(config_file), config_file_stamp(-1), pid(0), user(user), state(stopped),
          cooldown(0), cooldown_start(0), respawns(0), start_time(0), respawn_time(0)
{
    const char *stem = basename((char*)config_file.c_str());
    const char *ext = strstr(stem, ".conf");
    name = string(stem, ext ? ext - stem : strlen(stem));
    id = user->name + "/" + name;

    load_config();
}

void daemon::load_config()
{
    struct stat st = permissions::check(config_file, 0113, user->uid);
    if (st.st_mtime == config_file_stamp) return;

    map<string,string> config = parse_daemon_config(config_file);

    working_dir = key_exists(config, string("dir")) ? config["dir"] : "/";
    int uid = key_exists(config, string("dir")) ? uid_from_name(config["user"]) : user->uid;
    if (uid < 0) throw_str("%s is not allowed to run as unknown user %s in %s\n", user->name.c_str(), config["user"].c_str(), config_file.c_str());
    if (!user->can_run_as_uid[uid]) throw_str("%s is not allowed to run as %s in %s\n", user->name.c_str(), config["user"].c_str(), config_file.c_str());
    run_as_uid = uid;
    if (!key_exists(config, string("start"))) throw_str("Missing \"start\" in %s\n", config_file.c_str());
    start_command = config["start"];
    autostart = !key_exists(config, string("autostart")) || strchr("YyTt1Oo", config["autostart"].c_str()[0]);
    log_output = key_exists(config, string("output")) && config["output"] == "log";
    want_sockfile = key_exists(config, string("sockfile")) && strchr("YyTt1Oo", config["sockfile"].c_str()[0]);

    config_file_stamp = st.st_mtime;
}

bool daemon::exists()
{
    struct stat st;
    return stat(config_file.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

string daemon::sock_file()
{
    return "/var/run/daemon-manager/" + name_from_uid(run_as_uid) + "/" + id + ".socket";
}

static void mkdirs(string path, mode_t mode, int uid=-1, int gid=-1)
{
    mkdir(path.c_str(), mode);
    chown(path.c_str(), uid, gid);
}

void daemon::create_sock_dir()
{
    mkdirs("/var/run/daemon-manager/", 0755);
    mkdirs("/var/run/daemon-manager/" + name_from_uid(run_as_uid) + "/", 0770, run_as_uid);
    mkdirs("/var/run/daemon-manager/" + name_from_uid(run_as_uid) + "/" + user->name + "/", 0770, run_as_uid, user->gid);
}

void daemon::start(bool respawn)
{
    log(LOG_INFO, "Starting %s\n", id.c_str());

    load_config(); // Make sure we are up to date.

    int fd[2];
    if (pipe(fd) <0) throw_strerr("Couldn't pipe");
    fcntl(fd[0], F_SETFD, 1);
    fcntl(fd[1], F_SETFD, 1);
    int child = fork();
    if (child == -1) throw_strerr("Fork failed\n");
    if (child) {
        close(fd[1]);
        char err[1000]="";
        int red = read(fd[0], &err, sizeof(err));
        close(fd[0]);
        if(red > 0)
            throw string(err);
        pid = child; // Parent
        log(LOG_INFO, "Started %s. pid=%d\n", id.c_str(), pid);
        respawn_time = time(NULL);
        if (respawn)
            respawns++;
        else
            start_time = time(NULL);
        state = running;
        return;
    }

    // Child
    try {
        close(fd[0]);
        if (want_sockfile)
            create_sock_dir();
        setgid(user->gid)          == -1 && throw_strerr("Couldn't set gid to %d\n", user->gid);
        setuid(run_as_uid)         == -1 && throw_strerr("Couldn't set uid to %d (%s)", user->gid, user->name.c_str());
        chdir(working_dir.c_str()) == -1 && throw_strerr("Couldn't change to directory %s", working_dir.c_str());
        if (log_output) {
            struct stat st;
            if (stat(user->log_dir().c_str(), &st) != 0)
                mkdir(user->log_dir().c_str(), 0770) == -1 && throw_strerr("Couldn't create log directory %s", user->log_dir().c_str());
            close(1);
            close(2);
            string logfile=user->log_dir() + name + ".log";
            open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0750) == 1 || throw_strerr("Couldn't open log file %s", logfile.c_str());
            dup2(1,2) == -1 && throw_strerr("Couldn't dup stdout to stderr");
        }
        vector<string> elist;
        elist.push_back("HOME=" + user->homedir);
        elist.push_back("LOGNAME=" + user->name);
        elist.push_back("SHELL=/bin/sh");
        elist.push_back("PATH=/usr/bin:/bin");
        if (want_sockfile)
            elist.push_back("SOCK_FILE=" + sock_file());
        const char *env[elist.size()+1];
        for (size_t i=0; i<elist.size(); i++)
            env[i] = elist[i].c_str();
        env[elist.size()] = NULL;
        execle("/bin/sh", "/bin/sh", "-c", start_command.c_str(), (char*)NULL, env);
        throw_strerr("Couldn't exec");
    } catch (std::exception &e) {
        write(fd[1], e.what(), strlen(e.what())+1/*NULL*/);
        close(fd[1]);
        exit(0);
    }
}

void daemon::stop()
{
    if (pid) {
        log(LOG_INFO, "Stopping [%d] %s\n", pid, id.c_str());
        kill(pid, SIGTERM);
        state = stopping;
    }
    respawns = 0;
}


void daemon::respawn()
{
    reap();
    time_t now = time(NULL);
    time_t uptime = now - respawn_time;
    if (uptime < 60) cooldown = min((time_t)60, cooldown + 10); // back off if it's dying too often
    if (uptime > 60) cooldown = 0; // Clear cooldown on good behavior
    if (cooldown) {
        log(LOG_NOTICE, "%s is respawning too quickly, backing off. Cooldown time is %d seconds\n", id.c_str(), cooldown);
        cooldown_start = now;
        state = coolingdown;
    } else
        start(true);
}

void daemon::reap()
{
    pid = 0;
    state = stopped;
}

time_t daemon::cooldown_remaining()
{
    return max((time_t)0, cooldown - (time(NULL) - cooldown_start));
}

bool daemon_compare(class daemon *a, class daemon *b)
{
    return a->config_file < b->config_file;
}
