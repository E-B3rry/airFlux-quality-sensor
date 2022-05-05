import os
import time

import requests
import json


def fetch_data(ip="192.168.4.1", port=80):
    """
    Fetch the data from the server.
    :param ip: str
    :param port: int
    :return: The json parsed data from the server
    :rtype: dict
    """
    url = f"http://{str(ip)}:{str(port)}/api/fetchAndCleanCache"

    try:
        response = requests.get(url)
    except requests.exceptions.RequestException as e:
        return None

    if response.status_code == 200:
        return json.loads(response.content)
    else:
        return None


def parse_data_into_csv(data, filename="data.csv"):
    """
    Parse the data into a csv file.
    :param data: dict
    :param filename: str
    :return: Whether the file was written successfully or not
    :rtype: boolean
    """

    if data:
        if "error" in data or "currentLocalTime" not in data:
            return False

        ts_shift = int(data["currentLocalTime"])
        ts = int(time.time()) - ts_shift
        
        if not os.path.exists(filename):
            with open(filename, "w") as f:
                f.write("timestamp,air_quality\n")

        with open(filename, "a") as file:
            jsonData = data["data"].split(",")
            for item in jsonData:
                if not item:
                    break
		
                file.write(f"{str(ts + int(item.split(':')[0]))},{int(item.split(':')[1])}\n")

        return True
    return False


if __name__ == '__main__':
    print("Bienvenue sur le programme de rapatriement de données airFlux !")

    # Wait for user to connect to network
    print("Veuillez-vous connecter au réseau airFlux.135(...).")
    print("Appuyer sur entrée une fois connecté...\n\n")
    input()

    # Fetch data
    print("Récupération des données en cours...")
    data = fetch_data()

    if data:
        print("Données récupérées avec succès !")
        print("Conversion et enregistrement des données en cours...")

        try:
            parse_data_into_csv(data)
        except Exception as e:
            print(f"Une erreur est survenue lors de la conversion des données : {e}")
            print(f"Les données vont être perdues à la fermeture du programme. Si vous souhaitez les récupérer, appuyer sur entrée pour les afficher et les enregister dans \"backup.dat\".")
            input()
            print(data)
            with open("backup.dat", "w") as f:
                f.write(str(data))

            input()
            exit()

        print("Conversion et enregistrement des données terminées avec succès !")
        print("Les données se trouvent dans le fichier data.csv")
        print("\n\nMerci d'avoir utilisé le programme !")
        input()
    else:
        print("Erreur lors de la récupération des données.")
        input()
