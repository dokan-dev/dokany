using System;
using System.IO;
using System.Windows.Forms;

/* ChangePassphrase.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate changing the passphrase for a
	/// private key file instead of creating a new private key.
	/// A passphrase will be prompted if the given private-key has been
	/// encrypted.  After successfully loading the content of the
	/// private-key, the new passphrase will be prompted and the given
	/// private-key will be re-encrypted with that new passphrase.
	/// </summary>
	public class ChangePassphrase
	{
		public static void Main(String[] arg)
		{
			//Get the private key filename from the user
			Console.WriteLine("Please choose your private key file...");
			String pkey = InputForm.GetFileFromUser("Choose your privatekey(ex. ~/.ssh/id_dsa)");
			Console.WriteLine("You chose "+pkey+".");

			//Create a new JSch instance
			JSch jsch=new JSch();

			try
			{
				//Load the key pair
				KeyPair kpair=KeyPair.load(jsch, pkey);

				//Print the key file encryption status
				Console.WriteLine(pkey+" has "+(kpair.isEncrypted()?"been ":"not been ")+"encrypted");

				String passphrase = "";

				while(kpair.isEncrypted())
				{
					passphrase = InputForm.GetUserInput("Enter passphrase for "+pkey, true);
					if(!kpair.decrypt(passphrase))
					{
						Console.WriteLine("failed to decrypt "+pkey);
					}
					else
					{
						Console.WriteLine(pkey+" is decrypted.");
					}
				}

				passphrase="";

				passphrase=InputForm.GetUserInput("Enter new passphrase for "+pkey+
					       " (empty for no passphrase)", true);

				//Set the new passphrase
				kpair.setPassphrase(passphrase);
				//write the key to file
				kpair.writePrivateKey(pkey);
				//free the resource
				kpair.dispose();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}
	}
}
