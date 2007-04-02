	/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2007 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "configreader.h"
#include <algorithm>
#include <dirent.h>
#include <dlfcn.h>
#include "users.h"
#include "modules.h"
#include "wildcard.h"
#include "xline.h"
#include "socketengine.h"
#include "socket.h"
#include "command_parse.h"

bool InspIRCd::ULine(const char* server)
{
	if (!server)
		return false;
	if (!*server)
		return true;

	return (find(Config->ulines.begin(),Config->ulines.end(),server) != Config->ulines.end());
}

int InspIRCd::OperPassCompare(const char* data,const char* input, int tagnumber)
{
	int MOD_RESULT = 0;
	FOREACH_RESULT_I(this,I_OnOperCompare,OnOperCompare(data, input, tagnumber))
	if (MOD_RESULT == 1)
		return 0;
	if (MOD_RESULT == -1)
		return 1;
	return strcmp(data,input);
}

long InspIRCd::Duration(const char* str)
{
	char n_field[MAXBUF];
	long total = 0;
	n_field[0] = 0;

	if ((!strchr(str,'s')) && (!strchr(str,'m')) && (!strchr(str,'h')) && (!strchr(str,'d')) && (!strchr(str,'w')) && (!strchr(str,'y')))
	{
		std::string n = str;
		n += 's';
		return Duration(n.c_str());
	}
	
	for (char* i = (char*)str; *i; i++)
	{
		// if we have digits, build up a string for the value in n_field,
		// up to 10 digits in size.
		if ((*i >= '0') && (*i <= '9'))
		{
			strlcat(n_field,i,10);
		}
		else
		{
			// we dont have a digit, check for numeric tokens
			switch (tolower(*i))
			{
				case 's':
					total += atoi(n_field);
				break;

				case 'm':
					total += (atoi(n_field)*duration_m);
				break;

				case 'h':
					total += (atoi(n_field)*duration_h);
				break;

				case 'd':
					total += (atoi(n_field)*duration_d);
				break;

				case 'w':
					total += (atoi(n_field)*duration_w);
				break;

				case 'y':
					total += (atoi(n_field)*duration_y);
				break;
			}
			n_field[0] = 0;
		}
	}
	// add trailing seconds
	total += atoi(n_field);
	
	return total;
}

/* LoopCall is used to call a command classes handler repeatedly based on the contents of a comma seperated list.
 * There are two overriden versions of this method, one of which takes two potential lists and the other takes one.
 * We need a version which takes two potential lists for JOIN, because a JOIN may contain two lists of items at once,
 * the channel names and their keys as follows:
 * JOIN #chan1,#chan2,#chan3 key1,,key3
 * Therefore, we need to deal with both lists concurrently. The first instance of this method does that by creating
 * two instances of irc::commasepstream and reading them both together until the first runs out of tokens.
 * The second version is much simpler and just has the one stream to read, and is used in NAMES, WHOIS, PRIVMSG etc.
 * Both will only parse until they reach ServerInstance->Config->MaxTargets number of targets, to stop abuse via spam.
 */
int CommandParser::LoopCall(userrec* user, command_t* CommandObj, const char** parameters, int pcnt, unsigned int splithere, unsigned int extra)
{
	/* First check if we have more than one item in the list, if we don't we return zero here and the handler
	 * which called us just carries on as it was.
	 */
	if (!strchr(parameters[splithere],','))
		return 0;

	/** Some lame ircds will weed out dupes using some shitty O(n^2) algorithm.
	 * By using std::map (thanks for the idea w00t) we can cut this down a ton.
	 * ...VOOODOOOO!
	 */
	std::map<irc::string, bool> dupes;

	/* Create two lists, one for channel names, one for keys
	 */
	irc::commasepstream items1(parameters[splithere]);
	irc::commasepstream items2(parameters[extra]);
	std::string item = "*";
	unsigned int max = 0;

	/* Attempt to iterate these lists and call the command objech
	 * which called us, for every parameter pair until there are
	 * no more left to parse.
	 */
	while (((item = items1.GetToken()) != "") && (max++ < ServerInstance->Config->MaxTargets))
	{
		if (dupes.find(item.c_str()) == dupes.end())
		{
			const char* new_parameters[127];

			for (int t = 0; (t < pcnt) && (t < 127); t++)
				new_parameters[t] = parameters[t];

			std::string extrastuff = items2.GetToken();

			new_parameters[splithere] = item.c_str();
			new_parameters[extra] = extrastuff.c_str();

			CommandObj->Handle(new_parameters,pcnt,user);

			dupes[item.c_str()] = true;
		}
	}
	return 1;
}

int CommandParser::LoopCall(userrec* user, command_t* CommandObj, const char** parameters, int pcnt, unsigned int splithere)
{
	/* First check if we have more than one item in the list, if we don't we return zero here and the handler
	 * which called us just carries on as it was.
	 */
	if (!strchr(parameters[splithere],','))
		return 0;

	std::map<irc::string, bool> dupes;

	/* Only one commasepstream here */
	irc::commasepstream items1(parameters[splithere]);
	std::string item = "*";
	unsigned int max = 0;

	/* Parse the commasepstream until there are no tokens remaining.
	 * Each token we parse out, call the command handler that called us
	 * with it
	 */
	while (((item = items1.GetToken()) != "") && (max++ < ServerInstance->Config->MaxTargets))
	{
		if (dupes.find(item.c_str()) == dupes.end())
		{
			const char* new_parameters[127];

			for (int t = 0; (t < pcnt) && (t < 127); t++)
				new_parameters[t] = parameters[t];

			new_parameters[splithere] = item.c_str();

			parameters[splithere] = item.c_str();

			/* Execute the command handler over and over. If someone pulls our user
			 * record out from under us (e.g. if we /kill a comma sep list, and we're
			 * in that list ourselves) abort if we're gone.
			 */
			CommandObj->Handle(new_parameters,pcnt,user);

			dupes[item.c_str()] = true;
		}
	}
	/* By returning 1 we tell our caller that nothing is to be done,
	 * as all the previous calls handled the data. This makes the parent
	 * return without doing any processing.
	 */
	return 1;
}

bool CommandParser::IsValidCommand(const std::string &commandname, int pcnt, userrec * user)
{
	nspace::hash_map<std::string,command_t*>::iterator n = cmdlist.find(commandname);

	if (n != cmdlist.end())
	{
		if ((pcnt>=n->second->min_params) && (n->second->source != "<core>"))
		{
			if ((!n->second->flags_needed) || (user->modes[n->second->flags_needed-65]))
			{
				if (n->second->flags_needed)
				{
					return ((user->HasPermission(commandname)) || (ServerInstance->ULine(user->server)));
				}
				return true;
			}
		}
	}
	return false;
}

command_t* CommandParser::GetHandler(const std::string &commandname)
{
	nspace::hash_map<std::string,command_t*>::iterator n = cmdlist.find(commandname);
	if (n != cmdlist.end())
		return n->second;

	return NULL;
}

// calls a handler function for a command

CmdResult CommandParser::CallHandler(const std::string &commandname,const char** parameters, int pcnt, userrec *user)
{
	nspace::hash_map<std::string,command_t*>::iterator n = cmdlist.find(commandname);

	if (n != cmdlist.end())
	{
		if (pcnt >= n->second->min_params)
		{
			if ((!n->second->flags_needed) || (user->modes[n->second->flags_needed-65]))
			{
				if (n->second->flags_needed)
				{
					if ((user->HasPermission(commandname)) || (!IS_LOCAL(user)))
					{
						return n->second->Handle(parameters,pcnt,user);
					}
				}
				else
				{
					return n->second->Handle(parameters,pcnt,user);
				}
			}
		}
	}
	return CMD_INVALID;
}

void CommandParser::ProcessCommand(userrec *user, std::string &cmd)
{
	const char *command_p[127];
	int items = 0;
	irc::tokenstream tokens(cmd);
	std::string command;
	tokens.GetToken(command);

	while (tokens.GetToken(para[items]) && (items < 127))
	{
		command_p[items] = para[items].c_str();
		items++;
	}

	std::transform(command.begin(), command.end(), command.begin(), ::toupper);
		
	int MOD_RESULT = 0;
	FOREACH_RESULT(I_OnPreCommand,OnPreCommand(command,command_p,items,user,false,cmd));
	if (MOD_RESULT == 1) {
		return;
	}

	nspace::hash_map<std::string,command_t*>::iterator cm = cmdlist.find(command);
	
	if (cm != cmdlist.end())
	{
		if (user)
		{
			/* activity resets the ping pending timer */
			user->nping = ServerInstance->Time() + user->pingmax;
			if (cm->second->flags_needed)
			{
				if (!user->IsModeSet(cm->second->flags_needed))
				{
					user->WriteServ("481 %s :Permission Denied- You do not have the required operator privileges",user->nick);
					return;
				}
				if (!user->HasPermission(command))
				{
					user->WriteServ("481 %s :Permission Denied- Oper type %s does not have access to command %s",user->nick,user->oper,command.c_str());
					return;
				}
			}
			if ((user->registered == REG_ALL) && (!*user->oper) && (cm->second->IsDisabled()))
			{
				/* command is disabled! */
				user->WriteServ("421 %s %s :This command has been disabled.",user->nick,command.c_str());
				return;
			}
			if (items < cm->second->min_params)
			{
				user->WriteServ("461 %s %s :Not enough parameters.", user->nick, command.c_str());
				/* If syntax is given, display this as the 461 reply */
				if ((ServerInstance->Config->SyntaxHints) && (cm->second->syntax.length()))
					user->WriteServ("304 %s :SYNTAX %s %s", user->nick, cm->second->command.c_str(), cm->second->syntax.c_str());
				return;
			}
			if ((user->registered == REG_ALL) || (cm->second->WorksBeforeReg()))
			{
				/* ikky /stats counters */
				cm->second->use_count++;
				cm->second->total_bytes += cmd.length();

				int MOD_RESULT = 0;
				FOREACH_RESULT(I_OnPreCommand,OnPreCommand(command,command_p,items,user,true,cmd));
				if (MOD_RESULT == 1)
					return;

				/*
				 * WARNING: nothing may come after the
				 * command handler call, as the handler
				 * may free the user structure!
				 */
				CmdResult result = cm->second->Handle(command_p,items,user);

				FOREACH_MOD(I_OnPostCommand,OnPostCommand(command, command_p, items, user, result,cmd));
				return;
			}
			else
			{
				user->WriteServ("451 %s :You have not registered",command.c_str());
				return;
			}
		}
	}
	else if (user)
	{
		ServerInstance->stats->statsUnknown++;
		user->WriteServ("421 %s %s :Unknown command",user->nick,command.c_str());
	}
}

bool CommandParser::RemoveCommands(const char* source)
{
	nspace::hash_map<std::string,command_t*>::iterator i,safei;
	for (i = cmdlist.begin(); i != cmdlist.end(); i++)
	{
		safei = i;
		safei++;
		if (safei != cmdlist.end())
		{
			RemoveCommand(safei, source);
		}
	}
	safei = cmdlist.begin();
	if (safei != cmdlist.end())
	{
		RemoveCommand(safei, source);
	}
	return true;
}

void CommandParser::RemoveCommand(nspace::hash_map<std::string,command_t*>::iterator safei, const char* source)
{
	command_t* x = safei->second;
	if (x->source == std::string(source))
	{
		cmdlist.erase(safei);
	}
}

void CommandParser::ProcessBuffer(std::string &buffer,userrec *user)
{
	std::string::size_type a;

	if (!user)
		return;

	while ((a = buffer.rfind("\n")) != std::string::npos)
		buffer.erase(a);
	while ((a = buffer.rfind("\r")) != std::string::npos)
		buffer.erase(a);

	if (buffer.length())
	{
		if (!user->muted)
		{
			ServerInstance->Log(DEBUG,"-> :%s %s",user->nick,buffer.c_str());
			this->ProcessCommand(user,buffer);
		}
	}
}

bool CommandParser::CreateCommand(command_t *f, void* so_handle)
{
	if (so_handle)
	{
		if (RFCCommands.find(f->command) == RFCCommands.end())
			RFCCommands[f->command] = so_handle;
		else
		{
			ServerInstance->Log(DEFAULT,"ERK! Somehow, we loaded a cmd_*.so file twice! Only the first instance is being recorded.");
			return false;
		}
	}

	/* create the command and push it onto the table */
	if (cmdlist.find(f->command) == cmdlist.end())
	{
		cmdlist[f->command] = f;
		return true;
	}
	else return false;
}

CommandParser::CommandParser(InspIRCd* Instance) : ServerInstance(Instance)
{
	para.resize(128);
	this->SetupCommandTable();
}

bool CommandParser::FindSym(void** v, void* h)
{
	*v = dlsym(h, "init_command");
	const char* err = dlerror();
	if (err)
	{
		ServerInstance->Log(SPARSE, "Error loading core command: %s\n", err);
		return false;
	}
	return true;
}

bool CommandParser::ReloadCommand(const char* cmd)
{
	char filename[MAXBUF];
	char commandname[MAXBUF];
	int y = 0;

	for (const char* x = cmd; *x; x++, y++)
		commandname[y] = toupper(*x);

	commandname[y] = 0;

	SharedObjectList::iterator command = RFCCommands.find(commandname);

	if (command != RFCCommands.end())
	{
		command_t* cmdptr = cmdlist.find(commandname)->second;
		cmdlist.erase(cmdlist.find(commandname));

		for (char* x = commandname; *x; x++)
			*x = tolower(*x);


		delete cmdptr;
		dlclose(command->second);
		RFCCommands.erase(command);

		snprintf(filename, MAXBUF, "cmd_%s.so", commandname);
		this->LoadCommand(filename);

		return true;
	}

	return false;
}

CmdResult cmd_reload::Handle(const char** parameters, int pcnt, userrec *user)
{
	user->WriteServ("NOTICE %s :*** Reloading command '%s'",user->nick, parameters[0]);
	if (ServerInstance->Parser->ReloadCommand(parameters[0]))
	{
		user->WriteServ("NOTICE %s :*** Successfully reloaded command '%s'", user->nick, parameters[0]);
		ServerInstance->WriteOpers("*** RELOAD: %s reloaded the '%s' command.", user->nick, parameters[0]);
		return CMD_SUCCESS;
	}
	else
	{
		user->WriteServ("NOTICE %s :*** Could not reload command '%s'", user->nick, parameters[0]);
		return CMD_FAILURE;
	}
}

void CommandParser::LoadCommand(const char* name)
{
	char filename[MAXBUF];
	void* h;
	command_t* (*cmd_factory_func)(InspIRCd*);

	snprintf(filename, MAXBUF, "%s/%s", LIBRARYDIR, name);
	h = dlopen(filename, RTLD_NOW | RTLD_GLOBAL);

	if (!h)
	{
		ServerInstance->Log(SPARSE, "Error loading core command: %s", dlerror());
		return;
	}

	if (this->FindSym((void **)&cmd_factory_func, h))
	{
		command_t* newcommand = cmd_factory_func(ServerInstance);
		this->CreateCommand(newcommand, h);
	}
}

void CommandParser::SetupCommandTable()
{
	RFCCommands.clear();

	DIR* library = opendir(LIBRARYDIR);
	if (library)
	{
		dirent* entry = NULL;
		while ((entry = readdir(library)))
		{
			if (match(entry->d_name, "cmd_*.so"))
			{
				this->LoadCommand(entry->d_name);
			}
		}
		closedir(library);
	}

	this->CreateCommand(new cmd_reload(ServerInstance));
}

