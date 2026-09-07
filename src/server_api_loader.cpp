/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

void AddServerApiScripts();

// Keep the loader separate from the implementation so the module follows the
// standard AzerothCore script-loader convention.
void Addmod_server_apiScripts()
{
    AddServerApiScripts();
}
