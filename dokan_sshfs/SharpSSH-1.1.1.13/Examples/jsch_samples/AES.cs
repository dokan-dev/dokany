using System;
using System.IO;
using System.Windows.Forms;
using Tamir.SharpSsh.jsch;

/* AES.cs 
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */

namespace sharpSshTest.jsch_samples
{
	/// <summary>
	/// This program will demonstrate how to use "aes128-cbc" encryption.
	/// </summary>
	public class AES
	{
		public static void RunExample(String[] arg)
		{
			try
			{
				//Create a new JSch instance
				JSch jsch=new JSch();
			
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

				//Add AES128 as default cipher in the session config store
				System.Collections.Hashtable config=new System.Collections.Hashtable();
				config.Add("cipher.s2c", "aes128-cbc,3des-cbc");
				config.Add("cipher.c2s", "aes128-cbc,3des-cbc");
				session.setConfig(config);

				//Connect to remote SSH server
				session.connect();			

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
				Console.WriteLine(e.Message);
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
			/// </summary>\
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
