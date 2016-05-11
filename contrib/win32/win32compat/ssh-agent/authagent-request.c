/*
* Author: Manoj Ampalam <manoj.ampalam@microsoft.com>
* ssh-agent implementation on Windows
*
* Copyright (c) 2015 Microsoft Corp.
* All rights reserved
*
* Microsoft openssh win32 port
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
* NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
* THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <Ntsecapi.h>
#include <ntstatus.h>
#include "agent.h"
#include "agent-request.h"
#include "key.h"

static void 
InitLsaString(LSA_STRING *lsa_string, const char *str)
{
	if (str == NULL)
		memset(lsa_string, 0, sizeof(LSA_STRING));
	else {
		lsa_string->Buffer = str;
		lsa_string->Length = strlen(str);
		lsa_string->MaximumLength = lsa_string->Length + 1;
	}
}

static HANDLE 
generate_user_token(wchar_t* user) {
	HANDLE lsa_handle = 0, token = 0;;
	LSA_OPERATIONAL_MODE mode;
	ULONG auth_package_id;
	NTSTATUS ret, subStatus;
	KERB_S4U_LOGON *s4u_logon = NULL;
	size_t logon_info_size;
	LSA_STRING logon_process_name, auth_package_name, originName;
	TOKEN_SOURCE sourceContext;
	PKERB_INTERACTIVE_PROFILE pProfile = NULL;
	LUID            logonId;
	QUOTA_LIMITS    quotas;
	DWORD           cbProfile;
	
	InitLsaString(&logon_process_name, "ssh-agent");
	//InitLsaString(&auth_package_name, MICROSOFT_KERBEROS_NAME_A);
	InitLsaString(&auth_package_name, "Negotiate");
	InitLsaString(&originName, "sshd");
	if (ret = LsaRegisterLogonProcess(&logon_process_name, &lsa_handle, &mode) != STATUS_SUCCESS)
		goto done;

	if (ret = LsaLookupAuthenticationPackage(lsa_handle, &auth_package_name, &auth_package_id) != STATUS_SUCCESS)
		goto done;

	logon_info_size = sizeof(KERB_S4U_LOGON);
	logon_info_size += (wcslen(user) * 2 + 2);
	s4u_logon = malloc(logon_info_size);
	if (s4u_logon == NULL)
		goto done;

	s4u_logon->MessageType = KerbS4ULogon;
	s4u_logon->Flags = 0;
	s4u_logon->ClientUpn.Length = wcslen(user) * 2;
	s4u_logon->ClientUpn.MaximumLength = s4u_logon->ClientUpn.Length;
	s4u_logon->ClientUpn.Buffer = (WCHAR*)(s4u_logon + 1);
	memcpy(s4u_logon->ClientUpn.Buffer, user, s4u_logon->ClientUpn.Length + 2);
	s4u_logon->ClientRealm.Length = 0;
	s4u_logon->ClientRealm.MaximumLength = 0;
	s4u_logon->ClientRealm.Buffer = 0;

	memcpy(sourceContext.SourceName,".Jobs   ", sizeof(sourceContext.SourceName));

	if (AllocateLocallyUniqueId(&sourceContext.SourceIdentifier) != TRUE)
		goto done;

	if (ret = LsaLogonUser(lsa_handle, 
		&originName, 
		Network, 
		auth_package_id, 
		s4u_logon, 
		logon_info_size, 
		NULL, 
		&sourceContext,
		(PVOID*)&pProfile,
		&cbProfile,
		&logonId,
		&token,
		&quotas,
		&subStatus) != STATUS_SUCCESS)
		goto done;

done:
	if (lsa_handle)
		LsaDeregisterLogonProcess(lsa_handle);
	if (s4u_logon)
		free(s4u_logon);
	if (pProfile)
		LsaFreeReturnBuffer(pProfile);

	return token;
}

#define AUTH_REQUEST "keyauthenticate"
#define MAX_USER_NAME_LEN 255 + 255

int process_authagent_request(struct sshbuf* request, struct sshbuf* response, struct agent_connection* con) {
	int r = 0;
	char *opn, *key_blob, *user, *sig, *blob;
	size_t opn_len, key_blob_len, user_len, sig_len, blob_len;
	struct sshkey *key = NULL;
	HANDLE token = NULL, dup_token = NULL, client_proc = NULL;
	wchar_t wuser[MAX_USER_NAME_LEN];
	PWSTR wuser_home = NULL;
	ULONG client_pid;

	user = NULL;
	if ((r = sshbuf_get_string_direct(request, &opn, &opn_len)) != 0 ||
	    (r = sshbuf_get_string_direct(request, &key_blob, &key_blob_len)) != 0 ||
	    (r = sshbuf_get_cstring(request, &user, &user_len)) != 0 ||
	    (r = sshbuf_get_string_direct(request, &sig, &sig_len)) != 0 ||
	    (r = sshbuf_get_string_direct(request, &blob, &blob_len)) != 0 ||
	    (r = sshkey_from_blob(key_blob, key_blob_len, &key)) != 0)
		goto done;

	if ((opn_len != strlen(AUTH_REQUEST)) || (memcmp(opn, AUTH_REQUEST, opn_len) != 0)) {
		r = EINVAL;
		goto done;
	}

	if (0 == MultiByteToWideChar(CP_UTF8, 0, user, user_len + 1, wuser, MAX_USER_NAME_LEN)) {
		r = GetLastError();
		goto done;
	}

	if (key_verify(key, sig, sig_len, blob, blob_len) != 1 ||
	    (token = generate_user_token(wuser)) == 0 || 
	    (FALSE == GetNamedPipeClientProcessId(con->connection, &client_pid)) ||
	    ( (client_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid)) == NULL) ||
	    (FALSE == DuplicateHandle(GetCurrentProcess(), token, client_proc, &dup_token, TOKEN_QUERY | TOKEN_IMPERSONATE, FALSE, DUPLICATE_SAME_ACCESS)) ||
	    (sshbuf_put_u32(response, dup_token) != 0) ) {
		r = EINVAL;
		goto done;
	}
	
done:
	if (user)
		free(user);
	if (key)
		sshkey_free(key);
	if (token)
		CloseHandle(token);
	if (wuser_home)
		CoTaskMemFree(wuser_home);
	if (client_proc)
		CloseHandle(client_proc);
	return r;
}