using System;
using System.Collections;
using Tamir.SharpSsh;

namespace sharpSshTest.sharpssh_samples
{
	/// <summary>
	/// Summary description for SshExeTest.
	/// </summary>
	public class SshExeTest
	{
		public static void RunExample()
		{
			try
			{
				SshConnectionInfo input = Util.GetInput();
				SshExec exec = new SshExec(input.Host, input.User);
				if(input.Pass != null) exec.Password = input.Pass;
				if(input.IdentityFile != null) exec.AddIdentityFile( input.IdentityFile );

				Console.Write("Connecting...");
				exec.Connect();
				Console.WriteLine("OK");
				while(true)
				{
					Console.Write("Enter a command to execute ['Enter' to cancel]: ");
					string command = Console.ReadLine();
					if(command=="")break;
					string output = exec.RunCommand(command);				
					Console.WriteLine(output);
				}
				Console.Write("Disconnecting...");
				exec.Close();
				Console.WriteLine("OK");
			}
			catch(Exception e)
			{
				Console.WriteLine(e.Message);
			}
		}
	}
}
