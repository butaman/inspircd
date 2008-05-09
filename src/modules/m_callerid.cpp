#include "inspircd.h"
#include "users.h"
#include "channels.h"
#include "modules.h"

#include <set>

/* $ModDesc: Implementation of callerid (umode +g & /accept, ala hybrid etc) */

class callerid_data : public classbase
{
 public:
	time_t lastnotify;
	std::set<User*> accepting;

	callerid_data() : lastnotify(0) { }
};

callerid_data* GetData(User* who, bool extend = true)
{
	callerid_data* dat;
	if (who->GetExt("callerid_data", dat))
		return dat;
	else
	{
		if (extend)
		{
			dat = new callerid_data;
			who->Extend("callerid_data", dat);
			return dat;
		}
		else
			return NULL;
	}
}

void RemoveData(User* who)
{
	callerid_data* dat;
	who->GetExt("callerid_data", dat);

	if (!dat)
		return;

	who->Shrink("callerid_data");
	delete dat;
}

void RemoveFromAllAccepts(InspIRCd* ServerInstance, User* who)
{
	for (user_hash::iterator i = ServerInstance->Users->clientlist->begin(); i != ServerInstance->Users->clientlist->end(); ++i)
	{
		callerid_data* dat = GetData(i->second, false);
		
		if (!dat)
			continue;

		std::set<User*>& accepting = dat->accepting;
		std::set<User*>::iterator iter = accepting.find(who);

		if (iter == accepting.end())
			continue;

		accepting.erase(iter);
	}
}

class User_g : public SimpleUserModeHandler
{
public:
	User_g(InspIRCd* Instance) : SimpleUserModeHandler(Instance, 'g') { }
};

class CommandAccept : public Command
{
private:
	unsigned int& maxaccepts;
public:
	CommandAccept(InspIRCd* Instance, unsigned int& max) : Command(Instance, "ACCEPT", 0, 1), maxaccepts(max)
	{
		source = "m_callerid.so";
		syntax = "{[+|-]<nicks>}|*}";
	}

	/** Will take any number of nicks, which can be seperated by spaces, commas, or a mix.
	 * - in front of any nick removes, and an * lists. This effectively means you can do:
	 * /accept nick1,nick2,nick3,*
	 * to add 3 nicks and then show your list
	 */
	CmdResult Handle(const std::vector<std::string> &parameters, User* user)
	{
		/* Even if callerid mode is not set, we let them manage their ACCEPT list so that if they go +g they can
		 * have a list already setup. */
		bool atleastonechange = false;
		for (unsigned int i = 0; i < parameters.size(); ++i)
		{
			const char* arg = parameters[i].c_str();
			irc::commasepstream css(arg);
			std::string tok;

			while (css.GetToken(tok))
			{
				if (tok.empty())
					continue;

				if (tok == "*")
				{
					if (IS_LOCAL(user))
						continue;

					ListAccept(user);
				}
				else if (tok[0] == '-')
				{
					User* whotoremove = ServerInstance->FindNick(tok.substr(1));
					if (whotoremove)
						atleastonechange = RemoveAccept(user, whotoremove, false) || atleastonechange;
				}
				else
				{
					User* whotoadd = ServerInstance->FindNick(tok[0] == '+' ? tok.substr(1) : tok);
					if (whotoadd)
						atleastonechange = AddAccept(user, whotoadd, false) || atleastonechange;
					else
						user->WriteNumeric(401, "%s %s :No such nick/channel", user->nick, tok.c_str());
				}
			}
		}
		return atleastonechange ? CMD_FAILURE : CMD_SUCCESS;
	}

	void ListAccept(User* user)
	{
		callerid_data* dat = GetData(user, false);
		if (dat)
		{
			for (std::set<User*>::iterator i = dat->accepting.begin(); i != dat->accepting.end(); ++i)
				user->WriteNumeric(281, "%s %s", user->nick, (*i)->nick);
		}
		user->WriteNumeric(282, "%s :End of ACCEPT list", user->nick);
	}

	bool AddAccept(User* user, User* whotoadd, bool quiet)
	{
		callerid_data* dat = GetData(user, true);
		std::set<User*>& accepting = dat->accepting;
		if (accepting.size() >= maxaccepts)
		{
			if (!quiet)
				user->WriteNumeric(456, "%s :Accept list is full (limit is %d)", user->nick, maxaccepts);

			return false;
		}
		if (!accepting.insert(whotoadd).second)
		{
			if (!quiet)
				user->WriteNumeric(457, "%s %s :is already on your accept list", user->nick, whotoadd->nick);

			return false;
		}
		return true;
	}

	bool RemoveAccept(User* user, User* whotoremove, bool quiet)
	{
		callerid_data* dat = GetData(user, false);
		if (!dat)
		{
			if (!quiet)
				user->WriteNumeric(458, "%s %s :is not on your accept list", user->nick, whotoremove->nick);

			return false;
		}
		std::set<User*>& accepting = dat->accepting;
		std::set<User*>::iterator i = accepting.find(whotoremove);
		if (i == accepting.end())
		{
			if (!quiet)
				user->WriteNumeric(458, "%s %s :is not on your accept list", user->nick, whotoremove->nick);

			return false;
		}
		accepting.erase(i);
		return true;
	}
};

class ModuleCallerID : public Module
{
private:
	CommandAccept *mycommand;
	User_g* myumode;

	// Configuration variables:
	unsigned int maxaccepts; // Maximum ACCEPT entries.
	bool operoverride; // Operators can override callerid.
	bool tracknick; // Allow ACCEPT entries to update with nick changes.
	unsigned int notify_cooldown; // Seconds between notifications.

public:
	ModuleCallerID(InspIRCd* Me) : Module(Me)
	{
		OnRehash(NULL, "");
		mycommand = new CommandAccept(ServerInstance, maxaccepts);
		myumode = new User_g(ServerInstance);

		try
		{
			ServerInstance->AddCommand(mycommand);
		}
		catch (const ModuleException& e)
		{
			delete mycommand;
			throw ModuleException("Could not add command!");
		}
		if (!ServerInstance->Modes->AddMode(myumode))
		{
			delete mycommand;
			delete myumode;
			throw ModuleException("Could not add usermode +g");
		}
		Implementation eventlist[] = { I_OnRehash, I_OnUserPreNick, I_OnUserQuit, I_On005Numeric, I_OnUserPreNotice, I_OnUserPreMessage, I_OnCleanup };
		ServerInstance->Modules->Attach(eventlist, this, 7);
	}

	~ModuleCallerID()
	{
		ServerInstance->Modes->DelMode(myumode);
		delete myumode;
	}

	Version GetVersion()
	{
		return Version(1, 2, 0, 0, VF_COMMON | VF_VENDOR, API_VERSION);
	}

	void On005Numeric(std::string& output)
	{
		output += " CALLERID=g";
	}

	int PreText(User* user, User* dest, std::string& text, bool notice)
	{
		if (!dest->IsModeSet('g'))
			return 0;

		if (operoverride && IS_OPER(user))
			return 0;

		callerid_data* dat = GetData(dest, true);
		std::set<User*>& accepting = dat->accepting;
		time_t& lastnotify = dat->lastnotify;
		std::set<User*>::iterator i = accepting.find(dest);

		if (i == accepting.end())
		{
			time_t now = time(NULL);
			/* +g and *not* accepted */
			user->WriteNumeric(716, "%s %s :is in +g mode (server-side ignore).", user->nick, dest->nick);
			if (now > (lastnotify + (time_t)notify_cooldown))
			{
				user->WriteNumeric(717, "%s %s :has been informed that you messaged them.", user->nick, dest->nick);
				dest->WriteNumeric(718, "%s %s %s@%s :is messaging you, and you have umode +g", dest->nick, user->nick, user->ident, user->dhost);
				lastnotify = now;
			}
			return 1;
		}
		return 0;
	}

	int OnUserPreMessage(User* user, void* dest, int target_type, std::string& text, char status, CUList &exempt_list)
	{
		if (IS_LOCAL(user) && target_type == TYPE_USER)
			return PreText(user, (User*)dest, text, true);

		return 0;
	}

	int OnUserPreNotice(User* user, void* dest, int target_type, std::string& text, char status, CUList &exempt_list)
	{
		if (IS_LOCAL(user) && target_type == TYPE_USER)
			return PreText(user, (User*)dest, text, true);

		return 0;
	}

	void OnCleanup(int type, void* item)
	{
		if (type != TYPE_USER)
			return;

		User* u = (User*)item;
		/* Cleanup only happens on unload (before dtor), so keep this O(n) instead of O(n^2) which deferring to OnUserQuit would do.  */
		RemoveData(u);
	}

	int OnUserPreNick(User* user, const std::string& newnick)
	{
		if (!tracknick)
			RemoveFromAllAccepts(ServerInstance, user);
		return 0;
	}

	void OnUserQuit(User* user, const std::string& message, const std::string& oper_message)
	{
		RemoveData(user);
		RemoveFromAllAccepts(ServerInstance, user);
	}

	void OnRehash(User* user, const std::string& parameter)
	{
		ConfigReader Conf(ServerInstance);
		maxaccepts = Conf.ReadInteger("callerid", "maxaccepts", "16", 0, true);
		operoverride = Conf.ReadFlag("callerid", "operoverride", "0", 0);
		tracknick = Conf.ReadFlag("callerid", "tracknick", "0", 0);
		notify_cooldown = Conf.ReadInteger("callerid", "cooldown", "60", 0, true);
	}
};

MODULE_INIT(ModuleCallerID)

