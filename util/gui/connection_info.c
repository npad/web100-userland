/*  */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "web100.h"
#include "connection_info-int.h"


static int connection_info_find(web100_agent *_agent, const struct connection_info *_hints, struct connection_info **_res);
static int connection_info_refresh(web100_agent *_agent, struct connection_info **_info);


struct connection_info*
connection_info_head(web100_agent* agent) {
    static struct connection_info *res;
    struct connection_info *tmp;

    if(res) {
	// free the list
	while(res) {
	    tmp = res;
	    res = res->next;
	    free(tmp);
	}
    }

    connection_info_find(agent, NULL, &res);

    return res;
}

struct connection_info*
connection_info_next(struct connection_info *conninfo) {

    if(conninfo) return conninfo->next;
}

int
connection_info_get_cid(struct connection_info *conninfo) {
    if(conninfo) return conninfo->cid;
}

pid_t
connection_info_get_pid(struct connection_info *conninfo) {
    if(conninfo) return conninfo->pid;
}

uid_t
connection_info_get_uid(struct connection_info *conninfo) {
    if(conninfo) return conninfo->uid;
}

int
connection_info_get_state(struct connection_info *conninfo) {
    if(conninfo) return conninfo->state;
}

const char*
connection_info_get_cmdline(struct connection_info *conninfo) {
    if(conninfo) return (const char *) conninfo->cmdline;
}

WEB100_ADDRTYPE
connection_info_get_addrtype(struct connection_info *conninfo) {
    if(conninfo) return conninfo->addrtype;
}

int
connection_info_get_spec(struct connection_info *conninfo, struct web100_connection_spec *spec) {
    memcpy(spec, &conninfo->spec, sizeof (struct web100_connection_spec));
}

int
connection_info_get_spec_v6(struct connection_info *conninfo, struct web100_connection_spec_v6 *spec_v6) {
    memcpy(spec_v6, &conninfo->spec, sizeof (struct web100_connection_spec_v6));
}


static int
connection_info_find(web100_agent *agent, const struct connection_info *hints, struct connection_info **res)
{
    struct connection_info *conninfo;

    if(connection_info_refresh(agent, &conninfo)) {
	*res = NULL;
	return web100_errno;
    }

    *res = conninfo;
    return WEB100_ERR_SUCCESS;
}

static int
connection_info_refresh(web100_agent *agent, struct connection_info **conninfo)
{ 
    struct connection_info *cid_data, *tcp_data, *fd_data;
    struct connection_info *tmp, *tcp_head, *fd_head;
    struct connection_info *ci, *res;

    web100_connection *cp;
    web100_group *gp;
    web100_var *var;

    char buf[256], path[256];
    FILE *file, *file6; 
    int scan;
    DIR *dir, *fddir;
    struct dirent *direntp, *fddirentp;
    int fd;
    struct stat st;
    int stno;
    pid_t pid;
    int ii=0;
    int tcp_entry, fd_entry;

 
    // associate cid with IP 

    cid_data = NULL;
    cp = web100_connection_head(agent);
    while (cp) { 
	char *addr_name, *port_name;
	void *dst;

        if((ci = malloc(sizeof (struct connection_info))) == NULL) 
	    return WEB100_ERR_NOMEM;
	
        if((gp = web100_group_find(agent, "read")) == NULL) 
	    return WEB100_ERR_NOGROUP;

	if ((var = web100_var_find(gp, "LocalAddressType")) == NULL)
	    ci->addrtype = WEB100_ADDRTYPE_IPV4;
	else if (web100_raw_read(var, cp, &ci->addrtype) != WEB100_ERR_SUCCESS)
	    return web100_errno;

        if (strncmp(web100_get_agent_version(agent), "1.", 2) == 0) {
            addr_name = "RemoteAddress";
            port_name = "RemotePort";
        } else {
            addr_name = "RemAddress";
            port_name = "RemPort";
        }
        
        if ((var = web100_var_find(gp, "LocalAddress")) == NULL)
            return WEB100_ERR_FILE;
        if (web100_raw_read(var, cp, buf) != WEB100_ERR_SUCCESS)
            return web100_errno;
        if (ci->addrtype == WEB100_ADDRTYPE_IPV4)
            memcpy(&ci->spec.src_addr, buf, 4);
        else
            memcpy(&ci->spec_v6.src_addr, buf, 16);
        
        if ((var = web100_var_find(gp, addr_name)) == NULL)
            return WEB100_ERR_FILE;
        if (web100_raw_read(var, cp, buf) != WEB100_ERR_SUCCESS)
            return web100_errno;
        if (ci->addrtype == WEB100_ADDRTYPE_IPV4)
            memcpy(&ci->spec.dst_addr, buf, 4);
        else
            memcpy(&ci->spec_v6.dst_addr, buf, 16);
        
        if ((var = web100_var_find(gp, "LocalPort")) == NULL)
            return WEB100_ERR_FILE;
        dst = (ci->addrtype == WEB100_ADDRTYPE_IPV4) ? &ci->spec.src_port : &ci->spec_v6.src_port;
        if (web100_raw_read(var, cp, dst) != WEB100_ERR_SUCCESS)
            return web100_errno;
        
        if ((var = web100_var_find(gp, port_name)) == NULL)
            return WEB100_ERR_FILE;
        dst = (ci->addrtype == WEB100_ADDRTYPE_IPV4) ? &ci->spec.dst_port : &ci->spec_v6.dst_port;
        if (web100_raw_read(var, cp, dst) != WEB100_ERR_SUCCESS)
            return web100_errno;

        ci->cid = web100_get_connection_cid(cp);

        ci->next = cid_data;
        cid_data = ci;

        cp = web100_connection_next(cp); 
    } 
    
    // associate IP with ino

    file = fopen("/proc/net/tcp", "r");
    file6 = fopen("/proc/net/tcp6", "r");

    tcp_data = NULL;
    if(file) {
	while(fgets(buf, sizeof(buf), file) != NULL) { 
	    if((ci = malloc(sizeof (struct connection_info))) == NULL) {
		web100_errno = WEB100_ERR_NOMEM;
		return -WEB100_ERR_NOMEM;
	    }

	    if((scan = sscanf(buf, "%*u: %x:%x %x:%x %x %*x:%*x %*x:%*x %*x %u %*u %u",
			    (u_int32_t *) &(ci->spec.src_addr),
			    (u_int16_t *) &(ci->spec.src_port),
			    (u_int32_t *) &(ci->spec.dst_addr),
			    (u_int16_t *) &(ci->spec.dst_port),
			    (u_int *) &(ci->state),
			    (u_int *) &(ci->uid),
			    (u_int *) &(ci->ino))) == 7) { 
		ci->next = tcp_data; 
		tcp_data = ci; 
		tcp_data->addrtype = WEB100_ADDRTYPE_IPV4; 
	    } else {
		free(ci);
	    }
	}
	fclose(file);
    }

    if(file6) {
	while(fgets(buf, sizeof(buf), file6) != NULL) { 
	    if((ci = malloc(sizeof (struct connection_info))) == NULL) {
		web100_errno = WEB100_ERR_NOMEM;
		return -WEB100_ERR_NOMEM;
	    }

	    if((scan = sscanf(buf, "%*u: %s:%x %s:%x %x %*x:%*x %*x:%*x %*x %u %*u %u",
			    (char *) &(ci->spec.src_addr),
			    (u_int16_t *) &(ci->spec.src_port),
			    (char *) &(ci->spec.dst_addr),
			    (u_int16_t *) &(ci->spec.dst_port),
			    (u_int *) &(ci->state),
			    (u_int *) &(ci->uid),
			    (u_int *) &(ci->ino))) == 7) { 
		ci->next = tcp_data; 
		tcp_data = ci; 
		tcp_data->addrtype = WEB100_ADDRTYPE_IPV6;
	    } else {
		free(ci);
	    }
	}
	fclose(file6);
    }
    tcp_head = tcp_data;

    // associate ino with pid

    if(!(dir = opendir("/proc"))) { 
	web100_errno = WEB100_ERR_FILE;
       	return -WEB100_ERR_FILE;
    }

    fd_data = NULL;
    while((direntp = readdir(dir)) != NULL) {
        if((pid = atoi(direntp->d_name)) != 0)
       	{
	    sprintf(path, "%s/%d/%s/", "/proc", pid, "fd"); 
	    if(fddir = opendir(path)) { //else lacks permissions 
		while((fddirentp = readdir(fddir)) != NULL) 
		{ 
		    strcpy(buf, path);
		    strcat(buf, fddirentp->d_name); 
		    stno = stat(buf, &st); 
		    if(S_ISSOCK(st.st_mode)) // add new list entry
		    { 
			if((ci = malloc(sizeof(struct connection_info))) == NULL){
			    web100_errno = WEB100_ERR_NOMEM;
			    return -WEB100_ERR_NOMEM;
		       	}

			ci->ino = st.st_ino;
		       	ci->pid = pid; 

		       	ci->next = fd_data; 
		       	fd_data = ci; 

			sprintf(buf, "/proc/%d/status", pid);

			if((file = fopen(buf, "r")) == NULL) continue;

			if(fgets(buf, sizeof(buf), file) == NULL){
			    fclose(file);
			    continue;
		       	} 

			if(sscanf(buf, "Name: %s\n", &(fd_data->cmdline)) != 1){
			    fclose(file);
			    continue;
		       	} 
			fclose(file); 
		    }
	       	}
	    closedir(fddir);
	    }
       	}
    }
    fd_head = fd_data;
    closedir(dir); 

    //finally, collate above information 
 
    res = NULL;
    while(cid_data) {
	tcp_entry = 0;
	for(tcp_data = tcp_head; tcp_data; tcp_data = tcp_data->next) {
	    if ( ((tcp_data->addrtype == WEB100_ADDRTYPE_IPV4) &&

	          (tcp_data->spec.dst_port == cid_data->spec.dst_port &&
                   tcp_data->spec.dst_addr == cid_data->spec.dst_addr &&
                   tcp_data->spec.src_port == cid_data->spec.src_port)) ||

		 ((tcp_data->addrtype == WEB100_ADDRTYPE_IPV6) &&
		  (tcp_data->spec_v6.dst_port == cid_data->spec_v6.dst_port &&
           !strcmp(tcp_data->spec_v6.dst_addr, cid_data->spec_v6.dst_addr) &&
                   tcp_data->spec_v6.src_port == cid_data->spec_v6.src_port)) ) 
	    { 

		tcp_entry = 1;
		fd_entry = 0;
		for(fd_data = fd_head; fd_data; fd_data = fd_data->next) {
		    if(fd_data->ino == tcp_data->ino) { //then create entry 
			fd_entry = 1;

		       	ci = (struct connection_info *) malloc(sizeof (struct connection_info));
			bzero(ci, sizeof(struct connection_info));

			ci->pid = fd_data->pid; 
			strncpy(ci->cmdline, fd_data->cmdline, 256); 
			ci->uid = tcp_data->uid;
		       	ci->state = tcp_data->state; 

			ci->cid = cid_data->cid;
			ci->addrtype = cid_data->addrtype;

			if(ci->addrtype == WEB100_ADDRTYPE_IPV4)
			    memcpy(&(ci->spec), &cid_data->spec, sizeof (struct web100_connection_spec));
			if(ci->addrtype == WEB100_ADDRTYPE_IPV6)
			    memcpy(&(ci->spec_v6), &cid_data->spec_v6, sizeof (struct web100_connection_spec_v6));

			ci->next = res;
			res = ci;
		    }
		}
		if(!fd_entry) { // add entry w/out cmdline 
		    ci = (struct connection_info *) malloc(sizeof (struct connection_info));
		    bzero(ci, sizeof(struct connection_info));

		    ci->pid = 0;
		    strcpy(ci->cmdline, "");
		    ci->uid = tcp_data->uid;
		    ci->state = tcp_data->state;

		    ci->cid = cid_data->cid; 
		    ci->addrtype = cid_data->addrtype;

		    if(ci->addrtype == WEB100_ADDRTYPE_IPV4)
			memcpy(&(ci->spec), &cid_data->spec, sizeof (struct web100_connection_spec));
		    if(ci->addrtype == WEB100_ADDRTYPE_IPV6)
			memcpy(&(ci->spec_v6), &cid_data->spec_v6, sizeof (struct web100_connection_spec_v6));

		    ci->next = res;
		    res = ci;
		}
	    } 
	} 
	if(!tcp_entry) { // then connection has vanished; add residual cid info
                         // (only for consistency with entries in /proc/web100) 
	    ci = (struct connection_info *) malloc(sizeof (struct connection_info));
	    bzero(ci, sizeof(struct connection_info));

	    ci->cid = cid_data->cid; 
	    ci->addrtype = cid_data->addrtype;

	    if(ci->addrtype == WEB100_ADDRTYPE_IPV4)
		memcpy(&(ci->spec), &cid_data->spec, sizeof (struct web100_connection_spec));
	    if(ci->addrtype == WEB100_ADDRTYPE_IPV6)
		memcpy(&(ci->spec_v6), &cid_data->spec_v6, sizeof (struct web100_connection_spec_v6));

	    ci->next = res;
	    res = ci;
	}
	tmp = cid_data;
	cid_data = cid_data->next;
	free(tmp);
    }
    *conninfo = res;

    // free auxiliary lists

    while(tcp_head) {
        tcp_data = tcp_head;
	tcp_head = tcp_head->next;
	free(tcp_data);
    }
    while(fd_head) {
	fd_data = fd_head;
	fd_head = fd_head->next;
	free(fd_data);
    }

    return WEB100_ERR_SUCCESS;
}
