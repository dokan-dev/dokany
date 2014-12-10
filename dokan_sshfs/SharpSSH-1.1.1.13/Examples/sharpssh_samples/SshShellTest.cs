using System;
using System.Collections;
using Tamir.SharpSsh;
using System.IO;

namespace sharpSshTest.sharpssh_samples
{
	/// <summary>
	/// Summary description for SshExeTest.
	/// </summary>
	public class SshShellTest
	{
		public static void RunExample()
		{
			try
			{
				SshConnectionInfo input = Util.GetInput();
				SshShell shell = new SshShell(input.Host, input.User);
				if(input.Pass != null) shell.Password = input.Pass;
				if(input.IdentityFile != null) shell.AddIdentityFile( input.IdentityFile );

				//This statement must be prior to connecting
				shell.RedirectToConsole();

				Console.Write("Connecting...");
				shell.Connect();
				Console.WriteLine("OK");

				while(shell.ShellOpened)
				{
					System.Threading.Thread.Sleep(500);
				}
				Console.Write("Disconnecting...");
				shell.Close();
				Console.WriteLine("OK");
			}
			catch(Exception e)
			{
				Console.WriteLine(e.Message);
			}
		}
	}
}
