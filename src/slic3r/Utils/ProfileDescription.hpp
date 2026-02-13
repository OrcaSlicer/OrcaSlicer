#include <I18N.hpp>
#include <wx/string.h>
#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif

namespace ProfileDescrption {
    const std::string PROFILE_DESCRIPTION_0  = _L("It has a small layer height. This results in almost negligible layer lines and high print quality. It is suitable for most printing cases.");
    const std::string PROFILE_DESCRIPTION_1  = _L("Compared with the default profile of a 0.2 mm nozzle, it has lower speeds and acceleration, and the sparse infill pattern is Gyroid. This results in much higher print quality but a much longer print time.");
    const std::string PROFILE_DESCRIPTION_2  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a slightly bigger layer height. This results in almost negligible layer lines and slightly shorter print time.");
    const std::string PROFILE_DESCRIPTION_3  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a bigger layer height. This results in slightly visible layer lines but shorter print time.");
    const std::string PROFILE_DESCRIPTION_4  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a smaller layer height. This results in almost invisible layer lines and higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_5  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a smaller layer lines, lower speeds and acceleration, and the sparse infill pattern is Gyroid. This results in almost invisible layer lines and much higher print quality but much longer print time.");
    const std::string PROFILE_DESCRIPTION_6  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a smaller layer height. This results in minimal layer lines and higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_7  = _L("Compared with the default profile of a 0.2 mm nozzle, it has a smaller layer lines, lower speeds and acceleration, and the sparse infill pattern is Gyroid. This results in minimal layer lines and much higher print quality but much longer print time.");
    const std::string PROFILE_DESCRIPTION_8  = _L("It has a normal layer height. This results in average layer lines and print quality. It is suitable for most printing cases.");
    const std::string PROFILE_DESCRIPTION_9  = _L("Compared with the default profile of a 0.4 mm nozzle, it has more wall loops and a higher sparse infill density. This results in higher print strength but more filament consumption and longer print time.");
    const std::string PROFILE_DESCRIPTION_10 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a bigger layer height. This results in more apparent layer lines and lower print quality, but slightly shorter print time.");
    const std::string PROFILE_DESCRIPTION_11 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a bigger layer height. This results in more apparent layer lines and lower print quality, but shorter print time.");
    const std::string PROFILE_DESCRIPTION_12 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a smaller layer height. This results in less apparent layer lines and higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_13 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a smaller layer height, lower speeds and acceleration, and the sparse infill pattern is Gyroid. This results in less apparent layer lines and much higher print quality but much longer print time.");
    const std::string PROFILE_DESCRIPTION_14 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a smaller layer height. This results in almost negligible layer lines and higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_15 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a smaller layer height, lower speeds and acceleration, and the sparse infill pattern is Gyroid. This results in almost negligible layer lines and much higher print quality but much longer print time.");
    const std::string PROFILE_DESCRIPTION_16 = _L("Compared with the default profile of a 0.4 mm nozzle, it has a smaller layer height. This results in almost negligible layer lines and longer print time.");
    const std::string PROFILE_DESCRIPTION_17 = _L("It has a big layer height. This results in apparent layer lines and ordinary print quality and print time.");
    const std::string PROFILE_DESCRIPTION_18 = _L("Compared with the default profile of a 0.6 mm nozzle, it has more wall loops and a higher sparse infill density. This results in higher print strength but more filament consumption and longer print time.");
    const std::string PROFILE_DESCRIPTION_19 = _L("Compared with the default profile of a 0.6 mm nozzle, it has a bigger layer height. This results in more apparent layer lines and lower print quality, but shorter print time in some cases.");
    const std::string PROFILE_DESCRIPTION_20 = _L("Compared with the default profile of a 0.6 mm nozzle, it has a bigger layer height. This results in much more apparent layer lines and much lower print quality, but shorter print time in some cases.");
    const std::string PROFILE_DESCRIPTION_21 = _L("Compared with the default profile of a 0.6 mm nozzle, it has a smaller layer height. This results in less apparent layer lines and slight higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_22 = _L("Compared with the default profile of a 0.6 mm nozzle, it has a smaller layer height. This results in less apparent layer lines and higher print quality but longer print time.");
    const std::string PROFILE_DESCRIPTION_23 = _L("It has a very big layer height. This results in very apparent layer lines, low print quality and shorter print time.");
    const std::string PROFILE_DESCRIPTION_24 = _L("Compared with the default profile of a 0.8 mm nozzle, it has a bigger layer height. This results in very apparent layer lines and much lower print quality, but shorter print time in some cases.");
    const std::string PROFILE_DESCRIPTION_25 = _L("Compared with the default profile of a 0.8 mm nozzle, it has a much bigger layer height. This results in extremely apparent layer lines and much lower print quality, but much shorter print time in some cases.");
    const std::string PROFILE_DESCRIPTION_26 = _L("Compared with the default profile of a 0.8 mm nozzle, it has a slightly smaller layer height. This results in slightly less but still apparent layer lines and slightly higher print quality but longer print time in some cases.");
    const std::string PROFILE_DESCRIPTION_27 = _L("Compared with the default profile of a 0.8 mm nozzle, it has a smaller layer height. This results in less but still apparent layer lines and slightly higher print quality but longer print time in some cases.");
    const std::string PROFILE_DESCRIPTION_28 = _L("This is neither a commonly used filament, nor one of Bambu filaments, and it varies a lot from brand to brand. So, it's highly recommended to ask its vendor for suitable profile before printing and adjust some parameters according to its performances.");
    const std::string PROFILE_DESCRIPTION_29 = _L("When printing this filament, there's a risk of warping and low layer adhesion strength. To get better results, please refer to this wiki: Printing Tips for High Temp / Engineering materials.");
    const std::string PROFILE_DESCRIPTION_30 = _L("When printing this filament, there's a risk of nozzle clogging, oozing, warping and low layer adhesion strength. To get better results, please refer to this wiki: Printing Tips for High Temp / Engineering materials.");
    const std::string PROFILE_DESCRIPTION_31 = _L("To get better transparent or translucent results with the corresponding filament, please refer to this wiki: Printing tips for transparent PETG.");
    const std::string PROFILE_DESCRIPTION_32 = _L("To make the prints get higher gloss, please dry the filament before use, and set the outer wall speed to be 40 to 60 mm/s when slicing.");
    const std::string PROFILE_DESCRIPTION_33 = _L("This filament is only used to print models with a low density usually, and some special parameters are required. To get better printing quality, please refer to this wiki: Instructions for printing RC model with foaming PLA (PLA Aero).");
    const std::string PROFILE_DESCRIPTION_34 = _L("This filament is only used to print models with a low density usually, and some special parameters are required. To get better printing quality, please refer to this wiki: ASA Aero Printing Guide.");
    const std::string PROFILE_DESCRIPTION_35 = _L("This filament is too soft and not compatible with the AMS. Printing it is of many requirements, and to get better printing quality, please refer to this wiki: TPU printing guide.");
    const std::string PROFILE_DESCRIPTION_36 = _L("This filament has high enough hardness (about 67D)  and is compatible with the AMS. Printing it is of many requirements, and to get better printing quality, please refer to this wiki: TPU printing guide.");
    const std::string PROFILE_DESCRIPTION_37 = _L("If you are to print a kind of soft TPU, please don't slice with this profile, and it is only for TPU that has high enough hardness (not less than 55D) and is compatible with the AMS. To get better printing quality, please refer to this wiki: TPU printing guide.");
    const std::string PROFILE_DESCRIPTION_38 = _L("This is a water-soluble support filament, and usually it is only for the support structure and not for the model body. Printing this filament is of many requirements, and to get better printing quality, please refer to this wiki: PVA Printing Guide.");
    const std::string PROFILE_DESCRIPTION_39 = _L("This is a non-water-soluble support filament, and usually it is only for the support structure and not for the model body. To get better printing quality, please refer to this wiki: Printing Tips for Support Filament and Support Function.");
    const std::string PROFILE_DESCRIPTION_40 = _L("The generic presets are conservatively tuned for compatibility with a wider range of filaments. For higher printing quality and speeds, please use Bambu filaments with Bambu presets.");
    const std::string PROFILE_DESCRIPTION_41 = _L("High quality profile for 0.2mm nozzle, prioritizing print quality.");
    const std::string PROFILE_DESCRIPTION_42 = _L("High quality profile for 0.16mm layer height, prioritizing print quality and strength.");
    const std::string PROFILE_DESCRIPTION_43 = _L("Standard profile for 0.16mm layer height, prioritizing speed.");
    const std::string PROFILE_DESCRIPTION_44 = _L("High quality profile for 0.2mm layer height, prioritizing strength and print quality.");
    const std::string PROFILE_DESCRIPTION_45 = _L("Standard profile for 0.4mm nozzle, prioritizing speed.");
    const std::string PROFILE_DESCRIPTION_46 = _L("High quality profile for 0.6mm nozzle, prioritizing print quality and strength.");
    const std::string PROFILE_DESCRIPTION_47 = _L("Strength profile for 0.6mm nozzle, prioritizing strength.");
    const std::string PROFILE_DESCRIPTION_48 = _L("Standard profile for 0.6mm nozzle, prioritizing speed.");
    const std::string PROFILE_DESCRIPTION_49 = _L("High quality profile for 0.8mm nozzle, prioritizing print quality.");
    const std::string PROFILE_DESCRIPTION_50 = _L("Strength profile for 0.8mm nozzle, prioritizing strength.");
    const std::string PROFILE_DESCRIPTION_51 = _L("Standard profile for 0.8mm nozzle, prioritizing speed.");
}