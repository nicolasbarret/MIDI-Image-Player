# MIDI Image Player - Windows standalone

Application Windows autonome : affichage d'une image par classe de hauteur MIDI et retransmission MIDI vers un port virtuel tel que loopMIDI.

- Entrée MIDI physique sélectionnable.
- Sortie MIDI sélectionnable et MIDI Thru de tous les messages reçus.
- Douze images PNG/JPG/JPEG.
- Fenêtre de sortie noire au format 16:9, image centrée et proportionnelle.
- Fondu de zéro à cinq secondes au relâchement de la note.
- Sauvegarde des images et réglages dans le profil Windows.

## Routage
1. Créer dans loopMIDI un port nommé « MIDI vers FL Studio ».
2. Dans l'application, choisir le clavier physique comme entrée.
3. Choisir « MIDI vers FL Studio » comme sortie.
4. Dans FL Studio, choisir ce port virtuel comme entrée MIDI.
5. Charger les images et ouvrir la fenêtre de sortie OBS.

L'application ne produit aucun son.
