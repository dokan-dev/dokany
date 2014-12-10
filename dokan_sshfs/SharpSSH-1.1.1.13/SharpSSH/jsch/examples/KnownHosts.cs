using System;
using System.IO;
using System.Windows.Forms;

/* KnownHosts.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate the 'known_hosts' file handling.
	/// You will be asked username, hostname, a path for 'known_hosts' and passwd. 
	/// If everything works fine, you will get the shell prompt.
	/// In current implementation, jsch only reads 'known_hosts' for checking
	/// and does not modify it.
	/// </summary>
	public class KnownHosts
	{
		public static void Main(String[] arg)
		{
			try
			{
				//Get the "known hosts" filename from the user
				Console.WriteLine("Please select your 'known_hosts' from the poup window...");
				String file = InputForm.GetFileFromUser("Choose your known_hosts(ex. ~/.ssh/known_hosts)");
				Console.WriteLine("You chose "+file+".");
				//Create a new JSch instance
				JSch jsch=new JSch();
				//Set the known hosts file
				jsch.setKnownHosts(file);				

				//Get the KnownHosts repository from JSchs
				HostKeyRepository hkr=jsch.getHostKeyRepository();

				//Print all known hosts and keys
				HostKey[] hks=hkr.getHostKey();
				HostKey hk;
				if(hks!=null)
				{
					Console.WriteLine();
					Console.WriteLine("Host keys in "+hkr.getKnownHostsRepositoryID()+":");
					for(int i=0; i<hks.Length; i++)
					{
						hk=hks[i];
						Console.WriteLine(hk.getHost()+" "+
							hk.getType()+" "+
							hk.getFingerPrint(jsch));
					}
					Console.WriteLine("");
				}

				//Now connect to the remote server...

				//Prompt for username and server host
				Console.WriteLine("Please enter the user and host info at the popup window...");
				String host = InputForm.GetUserInput
					("Enter username@hostname",
					Environment.UserName+"@localhost");
				String user=host.Substring(0, host.IndexOf('@'));
				host=host.Substring(host.IndexOf('@')+1);

				//Create a new SSH session
				Session session=jsch.getSession(user, host, 22);

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);

				//Connect to remote SSH server
				session.connect();

				//Print the host key info
				//of the connected server:
				hk=session.getHostKey();
				Console.WriteLine("HostKey: "+
					hk.getHost()+" "+
					hk.getType()+" "+
					hk.getFingerPrint(jsch));
			
				//Open a new Shell channel on the SSH session
				Channel channel=session.openChannel("shell");

				//Redirect standard I/O to the SSH channel
				channel.setInputStream(Console.OpenStandardInput());
				channel.setOutputStream(Console.OpenStandardOutput());

				//Connect the channel
				channel.connect();

				Console.WriteLine("-- Shell channel is connected using the {0} cipher", 
					session.getCipher());

				//Wait till channel is closed
				while(!channel.isClosed())
				{
					System.Threading.Thread.Sleep(500);
				}

				//Disconnect from remote server
				channel.disconnect();
				session.disconnect();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		/// <summary>
		/// A user info for getting user data
		/// </summary>
		public class MyUserInfo : UserInfo
		{
			/// <summary>
			/// Holds the user password
			/// </summary>
			private String passwd;

			/// <summary>
			/// Returns the user password
			/// </summary>
			public String getPassword(){ return passwd; }

			/// <summary>
			/// Prompt the user for a Yes/No input
			/// </summary>
			public bool promptYesNo(String str)
			{
				return InputForm.PromptYesNo(str);
			}
			
			/// <summary>
			/// Returns the user passphrase (passwd for the private key file)
			/// </summary>
			public String getPassphrase(){ return null; }

			/// <summary>
			/// Prompt the user for a passphrase (passwd for the private key file)
			/// </summary>
			public bool promptPassphrase(String message){ return true; }

			/// <summary>
			/// Prompt the user for a password
			/// </summary>
			public bool promptPassword(String message)
			{
				passwd=InputForm.GetUserInput(message, true);
				return true;
			}

			/// <summary>
			/// Shows a message to the user
			/// </summary>
			public void showMessage(String message)
			{
				InputForm.ShowMessage(message);
			}
		}
	}
}
