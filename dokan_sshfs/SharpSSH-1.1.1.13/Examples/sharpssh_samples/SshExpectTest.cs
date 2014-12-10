using System;
using System.Collections;
using Tamir.SharpSsh;
using System.IO;

namespace sharpSshTest.sharpssh_samples
{
	/// <summary>
	/// Summary description for SshExeTest.
	/// </summary>
	public class SshExpectTest
	{
		public static void RunExample()
		{
			try
			{
				SshConnectionInfo input = Util.GetInput();
				SshShell ssh = new SshShell(input.Host, input.User);
				if(input.Pass != null) ssh.Password = input.Pass;
				if(input.IdentityFile != null) ssh.AddIdentityFile( input.IdentityFile );

				Console.Write("Connecting...");
				ssh.Connect();
				Console.WriteLine("OK");

				
				Console.Write("Enter a pattern to expect in response [e.g. '#', '$', C:\\\\.*>, etc...]: ");
				string pattern = Console.ReadLine();
				
				ssh.ExpectPattern = pattern;
				ssh.RemoveTerminalEmulationCharacters = true;
				
				Console.WriteLine();
				Console.WriteLine( ssh.Expect( pattern ) );

				while(ssh.ShellOpened)
				{	
					Console.WriteLine();
					Console.Write("Enter some data to write ['Enter' to cancel]: ");
					string data = Console.ReadLine();
					if(data=="")break;
					ssh.WriteLine(data);

					string output = ssh.Expect( pattern );
					Console.WriteLine( output );
				}

				Console.Write("Disconnecting...");
				ssh.Close();
				Console.WriteLine("OK");
			}
			catch(Exception e)
			{
				Console.WriteLine(e.Message);
			}
		}
	}
}
