/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2017 Peter Powell <petpow@saberuk.com>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/// $ModAuthor: Peter "SaberUK" Powell
/// $ModAuthorMail: petpow@saberuk.com
/// $ModDesc: Provides support for punishing users that send capitalised messages.
/// $ModDepends: core 3.0

#include "inspircd.h"
#include "modules/exemption.h"

enum
{
	ERR_INVALIDPARAM = 696
};

enum AntiCapsMethod
{
	ACM_BAN,
	ACM_BLOCK,
	ACM_MUTE,
	ACM_KICK,
	ACM_KICK_BAN
};

class AntiCapsSettings
{
 public:
	const AntiCapsMethod method;
	const uint16_t minlen;
	const uint8_t percent;

	AntiCapsSettings(AntiCapsMethod Method, uint16_t MinLen, uint8_t Percent)
		: method(Method)
		, minlen(MinLen)
		, percent(Percent)
	{
	}
};

class AntiCapsMode : public ParamMode<AntiCapsMode, SimpleExtItem<AntiCapsSettings> >
{
 private:
	bool ParseMethod(irc::sepstream& stream, AntiCapsMethod& method)
	{
		std::string methodstr;
		if (!stream.GetToken(methodstr))
			return false;

		if (methodstr == "ban")
			method = ACM_BAN;
		else if (methodstr == "block")
			method = ACM_BLOCK;
		else if (methodstr == "mute")
			method = ACM_MUTE;
		else if (methodstr == "kick")
			method = ACM_KICK;
		else if (methodstr == "kickban")
			method = ACM_KICK_BAN;
		else
			return false;

		return true;
	}

	bool ParseMinimumLength(irc::sepstream& stream, uint16_t& minlen)
	{
		std::string minlenstr;
		if (!stream.GetToken(minlenstr))
			return false;

		uint16_t result = atoi(minlenstr.c_str());
		if (result < 1 || result > ServerInstance->Config->Limits.MaxLine)
			return false;

		minlen = result;
		return true;
	}

	bool ParsePercent(irc::sepstream& stream, uint8_t& percent)
	{
		std::string percentstr;
		if (!stream.GetToken(percentstr))
			return false;

		int result = atoi(percentstr.c_str());
		if (result < 1 || result > 100)
			return false;

		percent = result;
		return true;
	}

 public:
	AntiCapsMode(Module* Creator)
		: ParamMode<AntiCapsMode, SimpleExtItem<AntiCapsSettings> >(Creator, "anticaps", 'B')
	{
	}

	ModeAction OnSet(User* source, Channel* channel, std::string& parameter)
	{
		irc::sepstream stream(parameter, ':');
		AntiCapsMethod method;
		uint16_t minlen;
		uint8_t percent;

		// Attempt to parse the method.
		if (!ParseMethod(stream, method) || !ParseMinimumLength(stream, minlen) || !ParsePercent(stream, percent))
		{
			source->WriteNumeric(ERR_INVALIDPARAM, channel, parameter, "Invalid anticaps parameter");
			return MODEACTION_DENY;
		}

		ext.set(channel, new AntiCapsSettings(method, minlen, percent));
		return MODEACTION_ALLOW;
	}

	void SerializeParam(Channel* chan, const AntiCapsSettings* acs, std::string& out)
	{
		switch (acs->method)
		{
			case ACM_BAN:
				out.append("ban");
				break;
			case ACM_BLOCK:
				out.append("block");
				break;
			case ACM_MUTE:
				out.append("mute");
				break;
			case ACM_KICK:
				out.append("kick");
				break;
			case ACM_KICK_BAN:
				out.append("kickban");
				break;
			default:
				out.append("unknown~");
				out.append(ConvToStr(acs->method));
				break;
		}
		out.push_back(':');
		out.append(ConvToStr(acs->minlen));
		out.push_back(':');
		out.append(ConvToStr(acs->percent));
	}
};

class ModuleAntiCaps : public Module
{
 private:
	CheckExemption::EventProvider exemptionprov;
	std::bitset<UCHAR_MAX> uppercase;
	std::bitset<UCHAR_MAX> lowercase;
	AntiCapsMode mode;

	void CreateBan(Channel* channel, User* user, bool mute)
	{
		std::string banmask(mute ? "m:" : "");
		banmask.append("*!*@");
		banmask.append(user->GetDisplayedHost());

		Modes::ChangeList changelist;
		changelist.push_add(ServerInstance->Modes->FindMode('b', MODETYPE_CHANNEL), banmask);
		ServerInstance->Modes->Process(ServerInstance->FakeClient, channel, NULL, changelist);
	}

	void InformUser(Channel* channel, User* user, const std::string& message)
	{
		user->WriteNumeric(ERR_CANNOTSENDTOCHAN, channel, message + " and was blocked.");
	}

 public:
	ModuleAntiCaps()
		: exemptionprov(this)
		, mode(this)
	{
	}

	void ReadConfig(ConfigStatus&) CXX11_OVERRIDE
	{
		ConfigTag* tag = ServerInstance->Config->ConfValue("anticaps");

		uppercase.reset();
		const std::string upper = tag->getString("uppercase", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
		for (std::string::const_iterator iter = upper.begin(); iter != upper.end(); ++iter)
			uppercase.set(*iter);

		lowercase.reset();
		const std::string lower = tag->getString("lowercase", "abcdefghijklmnopqrstuvwxyz");
		for (std::string::const_iterator iter = lower.begin(); iter != lower.end(); ++iter)
			lowercase.set(*iter);
	}

	ModResult OnUserPreMessage(User* user, void* dest, int target_type, std::string& text, char, CUList&, MessageType) CXX11_OVERRIDE
	{
		// We only want to operate on messages from local users.
		if (!IS_LOCAL(user))
			return MOD_RES_PASSTHRU;

		// The mode can only be applied to channels.
		if (target_type != TYPE_CHANNEL)
			return MOD_RES_PASSTHRU;

		// We only act if the channel has the mode set.
		Channel* channel = static_cast<Channel*>(dest);
		if (!channel->IsModeSet(&mode))
			return MOD_RES_PASSTHRU;

		// If the user is exempt from anticaps then we don't need
		// to do anything else.
		ModResult result;
		FIRST_MOD_RESULT_CUSTOM(exemptionprov, CheckExemption::EventListener, OnCheckExemption, result, (user, channel, "anticaps"));
		if (result == MOD_RES_ALLOW)
			return MOD_RES_PASSTHRU;

		// If the message is a CTCP then we skip it unless it is
		// an ACTION in which case we skip the prefix and suffix.
		std::string::const_iterator text_begin = text.begin();
		std::string::const_iterator text_end = text.end();
		if (text[0] == '\1')
		{
			// If the CTCP is not an action then skip it.
			if (text.compare(0, 8, "\1ACTION ", 8))
				return MOD_RES_PASSTHRU;

			// Skip the CTCP message characters.
			text_begin += 8;
			if (*text.rbegin() == '\1')
				text_end -= 1;
		}

		// Retrieve the anticaps config. This should never be
		// null but its better to be safe than sorry.
		AntiCapsSettings* config = mode.ext.get(channel);
		if (!config)
			return MOD_RES_PASSTHRU;

		// If the message is shorter than the minimum length then
		// we don't need to do anything else.
		size_t length = std::distance(text_begin, text_end);
		if (length < config->minlen)
			return MOD_RES_PASSTHRU;

		// Count the characters to see how many upper case and
		// ignored (non upper or lower) characters there are.
		size_t upper = 0;
		for (std::string::const_iterator iter = text_begin; iter != text_end; ++iter)
		{
			if (uppercase.test(*iter))
				upper += 1;
			else if (!lowercase.test(*iter))
				length -= 1;
		}

		// If the message was entirely symbols then the message
		// can't contain any upper case letters.
		if (length == 0)
			return MOD_RES_PASSTHRU;

		// Calculate the percentage.
		double percent = round((upper * 100) / length);
		if (percent < config->percent)
			return MOD_RES_PASSTHRU;

		std::string message = InspIRCd::Format("Your messsage exceeded the %d%% upper case character threshold for %s",
			config->percent, channel->name.c_str());

		switch (config->method)
		{
			case ACM_BAN:
				InformUser(channel, user, message);
				CreateBan(channel, user, false);
				break;

			case ACM_BLOCK:
				InformUser(channel, user, message);
				break;

			case ACM_MUTE:
				InformUser(channel, user, message);
				CreateBan(channel, user, true);
				break;

			case ACM_KICK:
				channel->KickUser(ServerInstance->FakeClient, user, message);
				break;

			case ACM_KICK_BAN:
				CreateBan(channel, user, false);
				channel->KickUser(ServerInstance->FakeClient, user, message);
				break;
		}
		return MOD_RES_DENY;
	}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides support for punishing users that send capitalised messages.", VF_COMMON);
	}
};

MODULE_INIT(ModuleAntiCaps)
