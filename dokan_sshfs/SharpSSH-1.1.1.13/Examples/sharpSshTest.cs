using System;
using Tamir.SharpSsh;
using Tamir.SharpSsh.jsch;
using System.Threading;
using System.Collections;
using System.IO;

/* 
 * ITransferProtocol.cs
 * 
 * Copyright (c) 2006 Tamir Gal, http://www.tamirgal.com, All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  	1. Redistributions of source code must retain the above copyright notice,
 *		this list of conditions and the following disclaimer.
 *
 *	    2. Redistributions in binary form must reproduce the above copyright 
 *		notice, this list of conditions and the following disclaimer in 
 *		the documentation and/or other materials provided with the distribution.
 *
 *	    3. The names of the authors may not be used to endorse or promote products
 *		derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR
 *  *OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 **/
namespace sharpSshTest
{
	/// <summary>
	/// Summary description for sharpSshTest.
	/// </summary>
	public class sharpSshTest
	{
		const int JSCH_SHELL			= 1;
		const int JSCH_AES				= 2;
		const int JSCH_PUBKEY			= 3;
		const int JSCH_SFTP				= 4;
		const int JSCH_KEYEGEN			= 5;
		const int JSCH_KNOWN_HOSTS		= 6;
		const int JSCH_CHANGE_PASS		= 7;
		const int JSCH_PORT_FWD_L		= 8;
		const int JSCH_PORT_FWD_R		= 9;
		const int JSCH_STREAM_FWD		= 10;
		const int JSCH_STREAM_SUBSYSTEM	= 11;
		const int JSCH_VIA_HTTP			= 12;
		const int SHARP_SHELL			= 13;
		const int SHARP_EXPECT			= 14;
		const int SHARP_EXEC			= 15;
		const int SHARP_TRANSFER		= 16;
		const int EXIT					= 17;

		public static void Main()
		{
			while(true)
			{
				PrintVersoin();
				Console.WriteLine();
				Console.WriteLine("JSch Smaples:");
				Console.WriteLine("=============");
				Console.WriteLine("{0})\tShell.cs",				JSCH_SHELL);
				Console.WriteLine("{0})\tAES.cs",				JSCH_AES);
				Console.WriteLine("{0})\tUserAuthPublicKey.cs", JSCH_PUBKEY);
				Console.WriteLine("{0})\tSftp.cs",				JSCH_SFTP);
				Console.WriteLine("{0})\tKeyGen.cs",			JSCH_KEYEGEN);
				Console.WriteLine("{0})\tKnownHosts.cs",		JSCH_KNOWN_HOSTS);
				Console.WriteLine("{0})\tChangePassphrase.cs",	JSCH_CHANGE_PASS);
				Console.WriteLine("{0})\tPortForwardingL.cs",	JSCH_PORT_FWD_L);
				Console.WriteLine("{0})\tPortForwardingR.cs",	JSCH_PORT_FWD_R);
				Console.WriteLine("{0})\tStreamForwarding.cs",	JSCH_STREAM_FWD);
				Console.WriteLine("{0})\tSubsystem.cs",	JSCH_STREAM_SUBSYSTEM);
				Console.WriteLine("{0})\tViaHTTP.cs",			JSCH_VIA_HTTP);
				Console.WriteLine();
				Console.WriteLine("SharpSSH Smaples:");
				Console.WriteLine("=================");
				Console.WriteLine("{0})\tSSH Shell sample",		SHARP_SHELL);
				Console.WriteLine("{0})\tSSH Expect Sample",	SHARP_EXPECT);
				Console.WriteLine("{0})\tSSH Exec Sample",		SHARP_EXEC);
				Console.WriteLine("{0})\tSSH File Transfer",	SHARP_TRANSFER);
				Console.WriteLine("{0})\tExit", EXIT);
				Console.WriteLine();

			INPUT:
				int i=-1;
				Console.Write("Please enter your choice: ");
				try
				{
					string input = Console.ReadLine();
					if (input=="") return;
					i = int.Parse( input  );
					Console.WriteLine();				
				}
				catch
				{
					i=-1;
				}

				switch(i)
				{
						//JSch samples:
					case JSCH_SHELL:
						jsch_samples.Shell.RunExample(null);
						break;
					case JSCH_AES:
						jsch_samples.AES.RunExample(null);;
						break;
					case JSCH_PUBKEY:
						jsch_samples.UserAuthPubKey.RunExample(null);
						break;
					case JSCH_SFTP:
						jsch_samples.Sftp.RunExample(null);
						break;
					case JSCH_KEYEGEN:
						jsch_samples.KeyGen.RunExample(GetArgs(new string[]{"Sig Type [rsa|dsa]", "output_keyfile", "comment"}));
						break;
					case JSCH_KNOWN_HOSTS:
						jsch_samples.KnownHosts.RunExample(null);
						break;
					case JSCH_CHANGE_PASS:
						jsch_samples.ChangePassphrase.RunExample(null);
						break;
					case JSCH_PORT_FWD_L:
						jsch_samples.PortForwardingL.RunExample(null);
						break;
					case JSCH_PORT_FWD_R:
						jsch_samples.PortForwardingR.RunExample(null);
						break;
					case JSCH_STREAM_FWD:
						jsch_samples.StreamForwarding.RunExample(null);
						break;
					case JSCH_STREAM_SUBSYSTEM:
						jsch_samples.Subsystem.RunExample(null);
						break;
					case JSCH_VIA_HTTP:
						jsch_samples.ViaHTTP.RunExample(null);
						break;

						//SharpSSH samples:
					case SHARP_SHELL:
						sharpssh_samples.SshShellTest.RunExample();
						break;
					case SHARP_EXPECT:
						sharpssh_samples.SshExpectTest.RunExample();
						break;
					case SHARP_EXEC:
						sharpssh_samples.SshExeTest.RunExample();
						break;
					case SHARP_TRANSFER:
						sharpssh_samples.SshFileTransferTest.RunExample();
						break;
					case EXIT:
						return;
					default:
						Console.Write("Bad input, ");
						goto INPUT;
				}
			}
		}

		public static string[] GetArgs(string[] args)
		{
			for(int i=0; i<args.Length; i++)
			{
				Console.Write("Enter {0}: ", args[i]);
				args[i] = Console.ReadLine();
			}
			return args;
		}

		static void PrintVersoin()
		{
			try
			{				
				System.Reflection.Assembly asm
					= System.Reflection.Assembly.GetAssembly(typeof(Tamir.SharpSsh.SshBase));
				Console.WriteLine("SharpSSH-"+asm.GetName().Version);
			}
			catch
			{
				Console.WriteLine("sharpSsh v1.0");
			}
		}

	}
}
